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

#ifndef MULTIPASS_DEFAULT_VM_IMAGE_VAULT_H
#define MULTIPASS_DEFAULT_VM_IMAGE_VAULT_H

#include <multipass/days.h>
#include <multipass/query.h>
#include <multipass/vm_image.h>
#include <multipass/vm_image_host.h>
#include <shared/base_vm_image_vault.h>

#include <QDir>
#include <QFuture>

#include <mutex>
#include <optional>
#include <unordered_map>

namespace multipass
{
class URLDownloader;
class VMImageHost;
class VaultRecord
{
public:
    multipass::VMImage image;
    multipass::Query query;
    std::chrono::system_clock::time_point last_accessed;
};
class DefaultVMImageVault final : public BaseVMImageVault
{
public:
    DefaultVMImageVault(std::vector<VMImageHost*> image_host, URLDownloader* downloader, multipass::Path cache_dir_path,
                        multipass::Path data_dir_path, multipass::days days_to_expire);
    ~DefaultVMImageVault();

    VMImage fetch_image(const FetchType& fetch_type, const Query& query, const PrepareAction& prepare,
                        const ProgressMonitor& monitor, const std::optional<std::string> checksum) override;
    void remove(const std::string& name) override;
    bool has_record_for(const std::string& name) override;
    void prune_expired_images() override;
    void update_images(const FetchType& fetch_type, const PrepareAction& prepare,
                       const ProgressMonitor& monitor) override;
    MemorySize minimum_image_size_for(const std::string& id) override;

private:
    VMImage image_instance_from(const std::string& name, const VMImage& prepared_image);
    VMImage download_and_prepare_source_image(const VMImageInfo& info, std::optional<VMImage>& existing_source_image,
                                              const QDir& image_dir, const FetchType& fetch_type,
                                              const PrepareAction& prepare, const ProgressMonitor& monitor);
    QString extract_image_from(const std::string& instance_name, const VMImage& source_image,
                               const ProgressMonitor& monitor);
    VMImage fetch_kernel_and_initrd(const VMImageInfo& info, const VMImage& source_image, const QDir& image_dir,
                                    const ProgressMonitor& monitor);
    std::optional<QFuture<VMImage>> get_image_future(const std::string& id);
    VMImage finalize_image_records(const Query& query, const VMImage& prepared_image, const std::string& id);
    VMImageInfo get_kernel_query_info(const std::string& name);
    void persist_image_records();
    void persist_instance_records();

    URLDownloader* const url_downloader;
    const QDir cache_dir;
    const QDir data_dir;
    const QDir instances_dir;
    const QDir images_dir;
    const days days_to_expire;
    std::mutex fetch_mutex;

    std::unordered_map<std::string, VaultRecord> prepared_image_records;
    std::unordered_map<std::string, VaultRecord> instance_image_records;
    std::unordered_map<std::string, QFuture<VMImage>> in_progress_image_fetches;
};
} // namespace multipass
#endif // MULTIPASS_DEFAULT_VM_IMAGE_VAULT_H
