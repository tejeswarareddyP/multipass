/*
 * Copyright (C) 2019-2022 Canonical, Ltd.
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

#include <multipass/exceptions/invalid_memory_size_exception.h>
#include <multipass/memory_size.h>
#include <multipass/utils.h>

#include <multipass/format.h>

#include <QRegularExpression>

namespace mp = multipass;

namespace
{
constexpr auto kibi = 1024LL;
constexpr auto mebi = kibi * kibi;
constexpr auto gibi = mebi * kibi;

long long in_bytes(const std::string& mem_value)
{
    QRegularExpression regex{QRegularExpression::anchoredPattern("(\\d+)(?:([KMG])(?:i?B)?|B)?"),
                             QRegularExpression::CaseInsensitiveOption};
    const auto matcher = regex.match(QString::fromStdString(mem_value)); // TODO accept decimals

    if (matcher.hasMatch())
    {
        auto val = matcher.captured(1).toLongLong(); // value is in the second capture (1st one is the whole match)
        const auto unit = matcher.captured(2);       // unit in the third capture (idem)
        if (!unit.isEmpty())
        {
            switch (unit.at(0).toLower().toLatin1())
            {
            case 'g':
                val *= gibi;
                break;
            case 'm':
                val *= mebi;
                break;
            case 'k':
                val *= kibi;
                break;
            default:
                assert(false && "Shouldn't be here (invalid unit)");
            }
        }

        return val;
    }

    throw mp::InvalidMemorySizeException{mem_value};
}
} // namespace

mp::MemorySize::MemorySize() : bytes{0LL}
{
}

mp::MemorySize::MemorySize(const std::string& val) : bytes{::in_bytes(val)}
{
}

long long mp::MemorySize::in_bytes() const noexcept
{
    return bytes;
}

long long mp::MemorySize::in_kilobytes() const noexcept
{
    return bytes / kibi; // integer division to floor
}

long long mp::MemorySize::in_megabytes() const noexcept
{
    return bytes / mebi; // integer division to floor
}

long long mp::MemorySize::in_gigabytes() const noexcept
{
    return bytes / gibi; // integer division to floor
}

bool mp::operator==(const MemorySize& a, const MemorySize& b)
{
    return a.bytes == b.bytes;
}

bool mp::operator!=(const MemorySize& a, const MemorySize& b)
{
    return a.bytes != b.bytes;
}

bool mp::operator<(const MemorySize& a, const MemorySize& b)
{
    return a.bytes < b.bytes;
}

bool mp::operator>(const MemorySize& a, const MemorySize& b)
{
    return a.bytes > b.bytes;
}

bool mp::operator<=(const MemorySize& a, const MemorySize& b)
{
    return a.bytes <= b.bytes;
}

bool mp::operator>=(const MemorySize& a, const MemorySize& b)
{
    return a.bytes >= b.bytes;
}

std::string mp::MemorySize::human_readable() const
{
    const auto giga = std::pair{gibi, "GiB"};
    const auto mega = std::pair{mebi, "MiB"};
    const auto kilo = std::pair{kibi, "KiB"};

    for (auto [unit, suffix] : {giga, mega, kilo})
        if (auto quotient = bytes / static_cast<float>(unit); quotient >= 1)
            return fmt::format("{:.1f}{}", quotient, suffix);

    return fmt::format("{}B", bytes);
}
