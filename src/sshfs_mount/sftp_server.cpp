/*
 * Copyright (C) 2017-2022 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "sftp_server.h"

#include <multipass/cli/client_platform.h>
#include <multipass/exceptions/exitless_sshprocess_exception.h>
#include <multipass/file_ops.h>
#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/platform.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/ssh/throw_on_error.h>
#include <multipass/utils.h>

#include <QDateTime>
#include <QDir>
#include <QFile>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto category = "sftp server";
using SftpHandleUPtr = std::unique_ptr<ssh_string_struct, void (*)(ssh_string)>;
using namespace std::literals::chrono_literals;

enum Permissions
{
    read_user = 0400,
    write_user = 0200,
    exec_user = 0100,
    read_group = 040,
    write_group = 020,
    exec_group = 010,
    read_other = 04,
    write_other = 02,
    exec_other = 01
};

auto make_sftp_session(ssh_session session, ssh_channel channel)
{
    mp::SftpServer::SftpSessionUptr sftp_server_session{sftp_server_new(session, channel), sftp_free};
    mp::SSH::throw_on_error(sftp_server_session, session, "[sftp] server init failed", sftp_server_init);
    return sftp_server_session;
}

int reply_ok(sftp_client_message msg)
{
    return sftp_reply_status(msg, SSH_FX_OK, nullptr);
}

int reply_failure(sftp_client_message msg)
{
    return sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
}

int reply_perm_denied(sftp_client_message msg)
{
    return sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "permission denied");
}

int reply_bad_handle(sftp_client_message msg, const char* type)
{
    return sftp_reply_status(msg, SSH_FX_BAD_MESSAGE, fmt::format("{}: invalid handle", type).c_str());
}

int reply_unsupported(sftp_client_message msg)
{
    return sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Unsupported message");
}

fmt::memory_buffer& operator<<(fmt::memory_buffer& buf, const char* v)
{
    fmt::format_to(std::back_inserter(buf), v);
    return buf;
}

auto longname_from(const QFileInfo& file_info, const std::string& filename)
{
    fmt::memory_buffer out;
    auto mode = file_info.permissions();

    if (file_info.isSymLink())
        out << "l";
    else if (file_info.isDir())
        out << "d";
    else
        out << "-";

    /* user */
    if (mode & QFileDevice::ReadOwner)
        out << "r";
    else
        out << "-";

    if (mode & QFileDevice::WriteOwner)
        out << "w";
    else
        out << "-";

    if (mode & QFileDevice::ExeOwner)
        out << "x";
    else
        out << "-";

    /*group*/
    if (mode & QFileDevice::ReadGroup)
        out << "r";
    else
        out << "-";

    if (mode & QFileDevice::WriteGroup)
        out << "w";
    else
        out << "-";

    if (mode & QFileDevice::ExeGroup)
        out << "x";
    else
        out << "-";

    /* other */
    if (mode & QFileDevice::ReadOther)
        out << "r";
    else
        out << "-";

    if (mode & QFileDevice::WriteOther)
        out << "w";
    else
        out << "-";

    if (mode & QFileDevice::ExeOther)
        out << "x";
    else
        out << "-";

    fmt::format_to(std::back_inserter(out), " 1 {} {} {}", file_info.ownerId(), file_info.groupId(), file_info.size());

    const auto timestamp = file_info.lastModified().toString("MMM d hh:mm:ss yyyy").toStdString();
    fmt::format_to(std::back_inserter(out), " {} {}", timestamp, filename);

    return out;
}

auto to_qt_permissions(uint32_t perms)
{
    QFile::Permissions out;

    if (perms & Permissions::read_user)
        out |= QFileDevice::ReadUser;
    if (perms & Permissions::write_user)
        out |= QFileDevice::WriteUser;
    if (perms & Permissions::exec_user)
        out |= QFileDevice::ExeUser;
    if (perms & Permissions::read_group)
        out |= QFileDevice::ReadGroup;
    if (perms & Permissions::write_group)
        out |= QFileDevice::WriteGroup;
    if (perms & Permissions::exec_group)
        out |= QFileDevice::ExeGroup;
    if (perms & Permissions::read_other)
        out |= QFileDevice::ReadOther;
    if (perms & Permissions::write_other)
        out |= QFileDevice::WriteOther;
    if (perms & Permissions::exec_other)
        out |= QFileDevice::ExeOther;

    return out;
}

auto to_unix_permissions(QFile::Permissions perms)
{
    int out = 0;

    if (perms & QFileDevice::ReadOwner)
        out |= Permissions::read_user;
    if (perms & QFileDevice::WriteOwner)
        out |= Permissions::write_user;
    if (perms & QFileDevice::ExeOwner)
        out |= Permissions::exec_user;
    if (perms & QFileDevice::ReadGroup)
        out |= Permissions::read_group;
    if (perms & QFileDevice::WriteGroup)
        out |= Permissions::write_group;
    if (perms & QFileDevice::ExeGroup)
        out |= Permissions::exec_group;
    if (perms & QFileDevice::ReadOther)
        out |= Permissions::read_other;
    if (perms & QFileDevice::WriteOther)
        out |= Permissions::write_other;
    if (perms & QFileDevice::ExeOther)
        out |= Permissions::exec_other;

    return out;
}

auto validate_path(const std::string& source_path, const std::string& current_path)
{
    if (source_path.empty())
        return false;

    return current_path.compare(0, source_path.length(), source_path) == 0;
}

template <typename T>
auto handle_from(sftp_client_message msg, const std::unordered_map<void*, std::unique_ptr<T>>& handles) -> T*
{
    const auto id = sftp_handle(msg->sftp, msg->handle);
    auto entry = handles.find(id);
    if (entry != handles.end())
        return entry->second.get();
    return nullptr;
}

void check_sshfs_status(mp::SSHSession& session, mp::SSHProcess& sshfs_process)
{
    try
    {
        if (sshfs_process.exit_code(250ms) != 0)
            throw std::runtime_error(sshfs_process.read_std_error());
    }
    catch (const mp::ExitlessSSHProcessException&)
    {
        // Timeout getting exit status; assume sshfs is running in the instance
    }
}

auto create_sshfs_process(mp::SSHSession& session, const std::string& sshfs_exec_line, const std::string& source,
                          const std::string& target)
{
    auto sshfs_process = session.exec(fmt::format("sudo {} :\"{}\" \"{}\"", sshfs_exec_line, source, target));

    check_sshfs_status(session, sshfs_process);

    return std::make_unique<mp::SSHProcess>(std::move(sshfs_process));
}

int mapped_id_for(const mp::id_mappings& id_maps, const int id, const int id_if_not_found)
{
    if (id == mp::no_id_info_available)
        return id_if_not_found;

    auto map = std::find_if(id_maps.cbegin(), id_maps.cend(), [id](std::pair<int, int> p) { return id == p.first; });

    if (map != id_maps.end())
    {
        if (map->second == mp::default_id)
            return id_if_not_found;
        else
            return map->second;
    }

    return id;
}

int reverse_id_for(const mp::id_mappings& id_maps, const int id, const int rev_id_if_not_found)
{
    auto found = std::find_if(id_maps.cbegin(), id_maps.cend(), [id](std::pair<int, int> p) { return id == p.second; });

    return found == id_maps.cend() ? rev_id_if_not_found : found->first;
}
} // namespace

mp::SftpServer::SftpServer(SSHSession&& session, const std::string& source, const std::string& target,
                           const id_mappings& gid_mappings, const id_mappings& uid_mappings, int default_uid,
                           int default_gid, const std::string& sshfs_exec_line)
    : ssh_session{std::move(session)},
      sshfs_process{create_sshfs_process(ssh_session, sshfs_exec_line, mp::utils::escape_char(source, '"'),
                                         mp::utils::escape_char(target, '"'))},
      sftp_server_session{make_sftp_session(ssh_session, sshfs_process->release_channel())},
      source_path{source},
      target_path{target},
      gid_mappings{gid_mappings},
      uid_mappings{uid_mappings},
      default_uid{default_uid},
      default_gid{default_gid},
      sshfs_exec_line{sshfs_exec_line}
{
}

mp::SftpServer::~SftpServer()
{
    stop_invoked = true;
}

sftp_attributes_struct mp::SftpServer::attr_from(const QFileInfo& file_info)
{
    sftp_attributes_struct attr{};

    attr.size = file_info.size();

    attr.uid = mapped_uid_for(file_info.ownerId());
    attr.gid = mapped_gid_for(file_info.groupId());

    attr.permissions = to_unix_permissions(file_info.permissions());
    attr.atime = file_info.lastRead().toUTC().toMSecsSinceEpoch() / 1000;
    attr.mtime = file_info.lastModified().toUTC().toMSecsSinceEpoch() / 1000;
    attr.flags =
        SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;

    if (file_info.isSymLink())
        attr.permissions |= SSH_S_IFLNK | 0777;
    else if (file_info.isDir())
        attr.permissions |= SSH_S_IFDIR;
    else if (file_info.isFile())
        attr.permissions |= SSH_S_IFREG;

    return attr;
}

inline int mp::SftpServer::mapped_uid_for(const int uid)
{
    return mapped_id_for(uid_mappings, uid, default_uid);
}

inline int mp::SftpServer::mapped_gid_for(const int gid)
{
    return mapped_id_for(gid_mappings, gid, default_gid);
}

inline int mp::SftpServer::reverse_uid_for(const int uid, const int rev_uid_if_not_found)
{
    return reverse_id_for(uid_mappings, uid, rev_uid_if_not_found);
}

inline int mp::SftpServer::reverse_gid_for(const int gid, const int rev_gid_if_not_found)
{
    return reverse_id_for(gid_mappings, gid, rev_gid_if_not_found);
}

void mp::SftpServer::process_message(sftp_client_message msg)
{
    int ret = 0;
    const auto type = sftp_client_message_get_type(msg);
    switch (type)
    {
    case SFTP_REALPATH:
        ret = handle_realpath(msg);
        break;
    case SFTP_OPENDIR:
        ret = handle_opendir(msg);
        break;
    case SFTP_MKDIR:
        ret = handle_mkdir(msg);
        break;
    case SFTP_RMDIR:
        ret = handle_rmdir(msg);
        break;
    case SFTP_LSTAT:
    case SFTP_STAT:
        ret = handle_stat(msg, type == SFTP_STAT);
        break;
    case SFTP_FSTAT:
        ret = handle_fstat(msg);
        break;
    case SFTP_READDIR:
        ret = handle_readdir(msg);
        break;
    case SFTP_CLOSE:
        ret = handle_close(msg);
        break;
    case SFTP_OPEN:
        ret = handle_open(msg);
        break;
    case SFTP_READ:
        ret = handle_read(msg);
        break;
    case SFTP_WRITE:
        ret = handle_write(msg);
        break;
    case SFTP_RENAME:
        ret = handle_rename(msg);
        break;
    case SFTP_REMOVE:
        ret = handle_remove(msg);
        break;
    case SFTP_SETSTAT:
    case SFTP_FSETSTAT:
        ret = handle_setstat(msg);
        break;
    case SFTP_READLINK:
        ret = handle_readlink(msg);
        break;
    case SFTP_SYMLINK:
        ret = handle_symlink(msg);
        break;
    case SFTP_EXTENDED:
        ret = handle_extended(msg);
        break;
    default:
        mpl::log(mpl::Level::trace, category, fmt::format("Unknown message: {}", static_cast<int>(type)));
        ret = reply_unsupported(msg);
    }
    if (ret != 0)
        mpl::log(mpl::Level::error, category, fmt::format("error occurred when replying to client: {}", ret));
}

void mp::SftpServer::run()
{
    using MsgUPtr = std::unique_ptr<sftp_client_message_struct, decltype(sftp_client_message_free)*>;

    while (true)
    {
        MsgUPtr client_msg{sftp_get_client_message(sftp_server_session.get()), sftp_client_message_free};
        auto msg = client_msg.get();
        if (msg == nullptr)
        {
            if (stop_invoked)
                break;

            int status{0};
            try
            {
                status = sshfs_process->exit_code(250ms);
            }
            catch (const mp::ExitlessSSHProcessException&)
            {
                status = 1;
            }

            if (status != 0)
            {
                mpl::log(mpl::Level::error, category,
                         "sshfs in the instance appears to have exited unexpectedly.  Trying to recover.");
                auto proc = ssh_session.exec(fmt::format("findmnt --source :{}  -o TARGET -n", source_path));
                auto mount_path = proc.read_std_output();
                if (!mount_path.empty())
                {
                    ssh_session.exec(fmt::format("sudo umount {}", mount_path));
                }

                sshfs_process =
                    create_sshfs_process(ssh_session, sshfs_exec_line, mp::utils::escape_char(source_path, '"'),
                                         mp::utils::escape_char(target_path, '"'));
                sftp_server_session = make_sftp_session(ssh_session, sshfs_process->release_channel());

                continue;
            }
            else
            {
                break;
            }
        }

        process_message(msg);
    }
}

void mp::SftpServer::stop()
{
    stop_invoked = true;
    ssh_session.force_shutdown();
}

int mp::SftpServer::handle_close(sftp_client_message msg)
{
    const auto id = sftp_handle(sftp_server_session.get(), msg->handle);

    auto erased = open_file_handles.erase(id);
    erased += open_dir_handles.erase(id);
    if (erased == 0)
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: bad handle requested", __FUNCTION__));
        return reply_bad_handle(msg, "close");
    }

    sftp_handle_remove(sftp_server_session.get(), id);
    return reply_ok(msg);
}

int mp::SftpServer::handle_fstat(sftp_client_message msg)
{
    auto file = handle_from(msg, open_file_handles);
    if (file == nullptr)
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: bad handle requested", __FUNCTION__));
        return reply_bad_handle(msg, "fstat");
    }

    QFileInfo file_info(*file);

    if (file_info.isSymLink())
        file_info = QFileInfo(file_info.symLinkTarget());

    auto attr = attr_from(file_info);
    return sftp_reply_attr(msg, &attr);
}

int mp::SftpServer::handle_mkdir(sftp_client_message msg)
{
    const auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    QDir dir(filename);
    if (!dir.mkdir(filename))
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: mkdir failed for \'{}\'", __FUNCTION__, filename));
        return reply_failure(msg);
    }

    QFile file(filename);
    if (!MP_FILEOPS.setPermissions(file, to_qt_permissions(msg->attr->permissions)))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: set permissions failed for \'{}\'", __FUNCTION__, filename));
        return reply_failure(msg);
    }

    QFileInfo current_dir(filename);
    QFileInfo parent_dir(current_dir.path());
    int rev_uid = reverse_uid_for(msg->attr->uid, parent_dir.ownerId());
    int rev_gid = reverse_gid_for(msg->attr->gid, parent_dir.groupId());

    if (MP_PLATFORM.chown(filename, rev_uid, rev_gid) < 0)
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("failed to chown '{}' to owner:{} and group:{}", filename, rev_uid, rev_gid));
        return reply_failure(msg);
    }
    return reply_ok(msg);
}

int mp::SftpServer::handle_rmdir(sftp_client_message msg)
{
    const auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    QDir dir(filename);
    if (!MP_FILEOPS.rmdir(dir, filename))
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: rmdir failed for \'{}\'", __FUNCTION__, filename));
        return reply_failure(msg);
    }

    return reply_ok(msg);
}

int mp::SftpServer::handle_open(sftp_client_message msg)
{
    const auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    QIODevice::OpenMode mode{QIODevice::NotOpen};
    const auto flags = sftp_client_message_get_flags(msg);
    if (flags & SSH_FXF_READ)
        mode |= QIODevice::ReadOnly;

    if (flags & SSH_FXF_WRITE)
    {
        mode |= QIODevice::WriteOnly;

        // This is needed to workaround an issue where sshfs does not pass through
        // O_APPEND.  This is fixed in sshfs v. 3.2.
        // Note: This goes against the default behavior of open().
        if (flags == SSH_FXF_WRITE)
        {
            mode |= QIODevice::Append;
            mpl::log(mpl::Level::info, category, "adding sshfs O_APPEND workaround");
        }
    }

    if (flags & SSH_FXF_APPEND)
        mode |= QIODevice::Append;

    if (flags & SSH_FXF_TRUNC)
        mode |= QIODevice::Truncate;

    auto file = std::make_unique<QFile>(filename);

    auto exists = QFileInfo(filename).isSymLink() || file->exists();

    if (!MP_FILEOPS.open(*file, mode))
    {
        mpl::log(mpl::Level::trace, category, fmt::format("Cannot open \'{}\': {}", filename, file->errorString()));
        return reply_failure(msg);
    }

    if (!exists)
    {
        if (!MP_FILEOPS.setPermissions(*file, to_qt_permissions(msg->attr->permissions)))
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("Cannot set permissions for \'{}\': {}", filename, file->errorString()));
            return reply_failure(msg);
        }

        QFileInfo current_file(filename);
        QFileInfo current_dir(current_file.path());

        auto new_uid = reverse_uid_for(msg->attr->uid, current_dir.ownerId());
        auto new_gid = reverse_gid_for(msg->attr->gid, current_dir.groupId());

        if (MP_PLATFORM.chown(filename, new_uid, new_gid) < 0)
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("failed to chown '{}' to owner:{} and group:{}", filename, new_uid, new_gid));
            return reply_failure(msg);
        }
    }

    SftpHandleUPtr sftp_handle{sftp_handle_alloc(sftp_server_session.get(), file.get()), ssh_string_free};
    if (!sftp_handle)
    {
        mpl::log(mpl::Level::trace, category, "Cannot allocate handle for open()");
        return reply_failure(msg);
    }

    open_file_handles.emplace(file.get(), std::move(file));

    return sftp_reply_handle(msg, sftp_handle.get());
}

int mp::SftpServer::handle_opendir(sftp_client_message msg)
{
    auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    QDir dir(filename);
    if (!dir.exists())
    {
        mpl::log(mpl::Level::trace, category, fmt::format("Cannot open directory \'{}\': no such directory", filename));
        return sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "no such directory");
    }

    if (!MP_FILEOPS.isReadable(dir))
    {
        mpl::log(mpl::Level::trace, category, fmt::format("Cannot read directory \'{}\': permission denied", filename));
        return reply_perm_denied(msg);
    }

    auto entry_list =
        std::make_unique<QFileInfoList>(dir.entryInfoList(QDir::AllEntries | QDir::System | QDir::Hidden));

    SftpHandleUPtr sftp_handle{sftp_handle_alloc(sftp_server_session.get(), entry_list.get()), ssh_string_free};
    if (!sftp_handle)
    {
        mpl::log(mpl::Level::trace, category, "Cannot allocate handle for opendir()");
        return reply_failure(msg);
    }

    open_dir_handles.emplace(entry_list.get(), std::move(entry_list));

    return sftp_reply_handle(msg, sftp_handle.get());
}

int mp::SftpServer::handle_read(sftp_client_message msg)
{
    auto file = handle_from(msg, open_file_handles);
    if (file == nullptr)
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: bad handle requested", __FUNCTION__));
        return reply_bad_handle(msg, "read");
    }

    const auto max_packet_size = 65536u;
    const auto len = std::min(msg->len, max_packet_size);

    std::vector<char> data;
    data.reserve(len);

    if (!MP_FILEOPS.seek(*file, msg->offset))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: cannot seek to position {} in \'{}\'", __FUNCTION__, msg->offset, file->fileName()));
        return reply_failure(msg);
    }

    auto r = MP_FILEOPS.read(*file, data.data(), len);
    if (r < 0)
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: read failed for {}: {}", __FUNCTION__, file->fileName(), file->errorString()));
        return sftp_reply_status(msg, SSH_FX_FAILURE, file->errorString().toStdString().c_str());
    }
    else if (r == 0)
        return sftp_reply_status(msg, SSH_FX_EOF, "End of file");

    return sftp_reply_data(msg, data.data(), r);
}

int mp::SftpServer::handle_readdir(sftp_client_message msg)
{
    auto dir_entries = handle_from(msg, open_dir_handles);
    if (dir_entries == nullptr)
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: bad handle requested", __FUNCTION__));
        return reply_bad_handle(msg, "readdir");
    }

    if (dir_entries->empty())
        return sftp_reply_status(msg, SSH_FX_EOF, nullptr);

    const auto max_num_entries_per_packet = 50ll;
    const auto num_entries = std::min(dir_entries->size(), max_num_entries_per_packet);

    for (int i = 0; i < num_entries; i++)
    {
        const QFileInfo entry = dir_entries->takeFirst();
        const auto filename = entry.fileName().toStdString();
        sftp_attributes_struct attr{};
        if (entry.isSymLink())
        {
            mp::platform::symlink_attr_from(entry.absoluteFilePath().toStdString().c_str(), &attr);
            attr.uid = mapped_uid_for(attr.uid);
            attr.gid = mapped_gid_for(attr.gid);
        }
        else
        {
            attr = attr_from(entry);
        }
        const auto longname = longname_from(entry, filename);
        sftp_reply_names_add(msg, filename.c_str(), longname.data(), &attr);
    }

    return sftp_reply_names(msg);
}

int mp::SftpServer::handle_readlink(sftp_client_message msg)
{
    auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    auto link = QFile::symLinkTarget(filename);
    if (link.isEmpty())
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: invalid link for \'{}\'", __FUNCTION__, filename));
        return sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "invalid link");
    }

    sftp_attributes_struct attr{};
    sftp_reply_names_add(msg, link.toStdString().c_str(), link.toStdString().c_str(), &attr);
    return sftp_reply_names(msg);
}

int mp::SftpServer::handle_realpath(sftp_client_message msg)
{
    auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    auto realpath = QFileInfo(filename).absoluteFilePath();
    return sftp_reply_name(msg, realpath.toStdString().c_str(), nullptr);
}

int mp::SftpServer::handle_remove(sftp_client_message msg)
{
    auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    QFile file{filename};
    if (!MP_FILEOPS.remove(file))
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: cannot remove \'{}\'", __FUNCTION__, filename));
        return reply_failure(msg);
    }

    return reply_ok(msg);
}

int mp::SftpServer::handle_rename(sftp_client_message msg)
{
    const auto source = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, source))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, source, source_path));
        return reply_perm_denied(msg);
    }

    if (!QFileInfo(source).isSymLink() && !QFile::exists(source))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: cannot rename \'{}\': no such file", __FUNCTION__, source));
        return sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "no such file");
    }

    const auto target = sftp_client_message_get_data(msg);
    if (!validate_path(source_path, target))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: cannot validate target path \'{}\' against source \'{}\'", __FUNCTION__, target,
                             source_path));
        return reply_perm_denied(msg);
    }

    QFile target_file{target};
    if (target_file.exists())
    {
        if (!MP_FILEOPS.remove(target_file))
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("{}: cannot remove \'{}\' for renaming", __FUNCTION__, target));
            return reply_failure(msg);
        }
    }

    QFile source_file{source};
    if (!MP_FILEOPS.rename(source_file, target))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: failed renaming \'{}\' to \'{}\'", __FUNCTION__, source, target));
        return reply_failure(msg);
    }

    return reply_ok(msg);
}

int mp::SftpServer::handle_setstat(sftp_client_message msg)
{
    QString filename;

    if (sftp_client_message_get_type(msg) == SFTP_FSETSTAT)
    {
        auto handle = handle_from(msg, open_file_handles);
        if (handle == nullptr)
        {
            mpl::log(mpl::Level::trace, category, fmt::format("{}: bad handle requested", __FUNCTION__));
            return reply_bad_handle(msg, "setstat");
        }

        filename = handle->fileName();
    }
    else
    {
        filename = sftp_client_message_get_filename(msg);
        if (!validate_path(source_path, filename.toStdString()))
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename,
                                 source_path));
            return reply_perm_denied(msg);
        }

        if (!QFileInfo(filename).isSymLink() && !QFile::exists(filename))
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("{}: cannot setstat \'{}\': no such file", __FUNCTION__, filename));
            return sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "no such file");
        }
    }

    QFile file{filename};

    if (msg->attr->flags & SSH_FILEXFER_ATTR_SIZE)
    {
        if (!MP_FILEOPS.resize(file, msg->attr->size))
        {
            mpl::log(mpl::Level::trace, category, fmt::format("{}: cannot resize \'{}\'", __FUNCTION__, filename));
            return reply_failure(msg);
        }
    }

    if (msg->attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS)
    {
        if (!MP_FILEOPS.setPermissions(file, to_qt_permissions(msg->attr->permissions)))
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("{}: set permissions failed for \'{}\'", __FUNCTION__, filename));
            return reply_failure(msg);
        }
    }

    if (msg->attr->flags & SSH_FILEXFER_ATTR_ACMODTIME)
    {
        if (MP_PLATFORM.utime(filename.toStdString().c_str(), msg->attr->atime, msg->attr->mtime) < 0)
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("{}: cannot set modification date for \'{}\'", __FUNCTION__, filename));
            return reply_failure(msg);
        }
    }

    if ((msg->attr->flags & SSH_FILEXFER_ATTR_UIDGID) &&
        (MP_PLATFORM.chown(filename.toStdString().c_str(), reverse_uid_for(msg->attr->uid, msg->attr->uid),
                           reverse_gid_for(msg->attr->gid, msg->attr->gid)) < 0))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: cannot set ownership for \'{}\'", __FUNCTION__, filename));
        return reply_failure(msg);
    }

    return reply_ok(msg);
}

int mp::SftpServer::handle_stat(sftp_client_message msg, const bool follow)
{
    auto filename = sftp_client_message_get_filename(msg);
    if (!validate_path(source_path, filename))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, filename, source_path));
        return reply_perm_denied(msg);
    }

    QFileInfo file_info(filename);
    if (!file_info.isSymLink() && !file_info.exists())
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: cannot stat  \'{}\': no such file", __FUNCTION__, filename));
        return sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "no such file");
    }

    sftp_attributes_struct attr{};

    if (!follow && file_info.isSymLink())
    {
        mp::platform::symlink_attr_from(filename, &attr);
        attr.uid = mapped_uid_for(attr.uid);
        attr.gid = mapped_gid_for(attr.gid);
    }
    else
    {
        if (file_info.isSymLink())
            file_info = QFileInfo(file_info.symLinkTarget());

        attr = attr_from(file_info);
    }

    return sftp_reply_attr(msg, &attr);
}

int mp::SftpServer::handle_symlink(sftp_client_message msg)
{
    const auto old_name = sftp_client_message_get_filename(msg);

    const auto new_name = sftp_client_message_get_data(msg);
    if (!validate_path(source_path, new_name))
    {
        mpl::log(
            mpl::Level::trace, category,
            fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, new_name, source_path));
        return reply_perm_denied(msg);
    }

    if (!MP_PLATFORM.symlink(old_name, new_name, QFileInfo(old_name).isDir()))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: failure creating symlink from \'{}\' to \'{}\'", __FUNCTION__, old_name, new_name));
        return reply_failure(msg);
    }

    return reply_ok(msg);
}

int mp::SftpServer::handle_write(sftp_client_message msg)
{
    auto file = handle_from(msg, open_file_handles);
    if (file == nullptr)
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: bad handle requested", __FUNCTION__));
        return reply_bad_handle(msg, "write");
    }

    auto len = ssh_string_len(msg->data);
    auto data_ptr = ssh_string_get_char(msg->data);
    if (!MP_FILEOPS.seek(*file, msg->offset))
    {
        mpl::log(mpl::Level::trace, category,
                 fmt::format("{}: cannot seek to position {} in \'{}\'", __FUNCTION__, msg->offset, file->fileName()));
        return reply_failure(msg);
    }

    do
    {
        auto r = MP_FILEOPS.write(*file, data_ptr, len);
        if (r < 0)
        {
            mpl::log(
                mpl::Level::trace, category,
                fmt::format("{}: write failed for \'{}\': {}", __FUNCTION__, file->fileName(), file->errorString()));
            return reply_failure(msg);
        }

        file->flush();

        data_ptr += r;
        len -= r;
    } while (len > 0);

    return reply_ok(msg);
}

int mp::SftpServer::handle_extended(sftp_client_message msg)
{
    const auto submessage = sftp_client_message_get_submessage(msg);
    if (submessage == nullptr)
    {
        mpl::log(mpl::Level::trace, category, fmt::format("{}: invalid submesage requested", __FUNCTION__));
        return reply_failure(msg);
    }

    const std::string method(submessage);
    if (method == "hardlink@openssh.com")
    {
        const auto old_name = sftp_client_message_get_filename(msg);

        const auto new_name = sftp_client_message_get_data(msg);
        if (!validate_path(source_path, new_name))
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("{}: cannot validate path \'{}\' against source \'{}\'", __FUNCTION__, new_name,
                                 source_path));
            return reply_perm_denied(msg);
        }

        if (!MP_PLATFORM.link(old_name, new_name))
        {
            mpl::log(mpl::Level::trace, category,
                     fmt::format("{}: failed creating link from \'{}\' to \'{}\'", __FUNCTION__, old_name, new_name));
            return reply_failure(msg);
        }
    }
    else if (method == "posix-rename@openssh.com")
    {
        return handle_rename(msg);
    }
    else
    {
        mpl::log(mpl::Level::trace, category, fmt::format("Unhandled extended method requested: {}", method));
        return reply_unsupported(msg);
    }

    return reply_ok(msg);
}
