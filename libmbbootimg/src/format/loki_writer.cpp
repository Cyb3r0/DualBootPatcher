/*
 * Copyright (C) 2015-2018  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of DualBootPatcher
 *
 * DualBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * DualBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with DualBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mbbootimg/format/loki_writer_p.h"

#include <algorithm>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <openssl/sha.h>

#include "mbcommon/endian.h"
#include "mbcommon/file.h"
#include "mbcommon/file_util.h"
#include "mbcommon/finally.h"

#include "mbbootimg/entry.h"
#include "mbbootimg/format/align_p.h"
#include "mbbootimg/format/android_error.h"
#include "mbbootimg/format/loki_defs.h"
#include "mbbootimg/format/loki_error.h"
#include "mbbootimg/format/loki_p.h"
#include "mbbootimg/header.h"
#include "mbbootimg/writer_p.h"

namespace mb::bootimg::loki
{

constexpr size_t MAX_ABOOT_SIZE = 2 * 1024 * 1024;

LokiFormatWriter::LokiFormatWriter() noexcept
    : FormatWriter()
    , m_hdr()
    , m_sha_ctx()
{
}

LokiFormatWriter::~LokiFormatWriter() noexcept = default;

Format LokiFormatWriter::type()
{
    return Format::Loki;
}

oc::result<void> LokiFormatWriter::open(File &file)
{
    (void) file;

    if (!SHA1_Init(&m_sha_ctx)) {
        return android::AndroidError::Sha1InitError;
    }

    m_seg = SegmentWriter();

    return oc::success();
}

oc::result<void> LokiFormatWriter::close(File &file)
{
    auto reset_state = finally([&] {
        m_hdr = {};
        m_aboot.clear();
        m_sha_ctx = {};
        m_seg = {};
    });

    if (m_seg) {
        auto swentry = m_seg->entry();

        // If successful, finish up the boot image
        if (swentry == m_seg->entries().end()) {
            OUTCOME_TRY(file_size, file.seek(0, SEEK_CUR));

            // Truncate to set size
            OUTCOME_TRYV(file.truncate(file_size));

            // Set ID
            unsigned char digest[SHA_DIGEST_LENGTH];
            if (!SHA1_Final(digest, &m_sha_ctx)) {
                return android::AndroidError::Sha1UpdateError;
            }
            memcpy(m_hdr.id, digest, SHA_DIGEST_LENGTH);

            // Convert fields back to little-endian
            android_fix_header_byte_order(m_hdr);

            // Seek back to beginning to write header
            OUTCOME_TRYV(file.seek(0, SEEK_SET));

            // Write header
            OUTCOME_TRYV(file_write_exact(file, &m_hdr, sizeof(m_hdr)));

            // Patch with Loki
            OUTCOME_TRYV(_loki_patch_file(file, m_aboot.data(),
                                          m_aboot.size()));
        }
    }

    return oc::success();
}

oc::result<Header> LokiFormatWriter::get_header(File &file)
{
    (void) file;

    Header header;
    header.set_supported_fields(NEW_SUPPORTED_FIELDS);

    return std::move(header);
}

oc::result<void>
LokiFormatWriter::write_header(File &file, const Header &header)
{
    // Construct header
    m_hdr = {};
    memcpy(m_hdr.magic, android::BOOT_MAGIC, android::BOOT_MAGIC_SIZE);

    if (auto address = header.kernel_address()) {
        m_hdr.kernel_addr = *address;
    }
    if (auto address = header.ramdisk_address()) {
        m_hdr.ramdisk_addr = *address;
    }
    if (auto address = header.secondboot_address()) {
        m_hdr.second_addr = *address;
    }
    if (auto address = header.kernel_tags_address()) {
        m_hdr.tags_addr = *address;
    }
    if (auto page_size = header.page_size()) {
        switch (*page_size) {
        case 2048:
        case 4096:
        case 8192:
        case 16384:
        case 32768:
        case 65536:
        case 131072:
            m_hdr.page_size = *page_size;
            break;
        default:
            //DEBUG("Invalid page size: %" PRIu32, *page_size);
            return android::AndroidError::InvalidPageSize;
        }
    } else {
        return android::AndroidError::MissingPageSize;
    }

    if (auto board_name = header.board_name()) {
        if (board_name->size() >= sizeof(m_hdr.name)) {
            return android::AndroidError::BoardNameTooLong;
        }

        strncpy(reinterpret_cast<char *>(m_hdr.name), board_name->c_str(),
                sizeof(m_hdr.name) - 1);
        m_hdr.name[sizeof(m_hdr.name) - 1] = '\0';
    }
    if (auto cmdline = header.kernel_cmdline()) {
        if (cmdline->size() >= sizeof(m_hdr.cmdline)) {
            return android::AndroidError::KernelCmdlineTooLong;
        }

        strncpy(reinterpret_cast<char *>(m_hdr.cmdline), cmdline->c_str(),
                sizeof(m_hdr.cmdline) - 1);
        m_hdr.cmdline[sizeof(m_hdr.cmdline) - 1] = '\0';
    }

    // TODO: UNUSED
    // TODO: ID

    std::vector<SegmentWriterEntry> entries;

    entries.push_back({ EntryType::Kernel, 0, {}, m_hdr.page_size });
    entries.push_back({ EntryType::Ramdisk, 0, {}, m_hdr.page_size });
    entries.push_back({ EntryType::DeviceTree, 0, {}, m_hdr.page_size });
    entries.push_back({ EntryType::Aboot, 0, 0, 0 });

    OUTCOME_TRYV(m_seg->set_entries(std::move(entries)));

    // Start writing after first page
    OUTCOME_TRYV(file.seek(m_hdr.page_size, SEEK_SET));

    return oc::success();
}

oc::result<Entry> LokiFormatWriter::get_entry(File &file)
{
    return m_seg->get_entry(file);
}

oc::result<void> LokiFormatWriter::write_entry(File &file, const Entry &entry)
{
    return m_seg->write_entry(file, entry);
}

oc::result<size_t>
LokiFormatWriter::write_data(File &file, const void *buf, size_t buf_size)
{
    auto swentry = m_seg->entry();

    if (swentry->type == EntryType::Aboot) {
        if (buf_size > MAX_ABOOT_SIZE - m_aboot.size()) {
            return LokiError::AbootImageTooLarge;
        }

        size_t old_aboot_size = m_aboot.size();
        m_aboot.resize(old_aboot_size + buf_size);

        memcpy(m_aboot.data() + old_aboot_size, buf, buf_size);

        return buf_size;
    } else {
        OUTCOME_TRY(n, m_seg->write_data(file, buf, buf_size));

        // We always include the image in the hash. The size is sometimes
        // included and is handled in finish_entry().
        if (!SHA1_Update(&m_sha_ctx, buf, n)) {
            return android::AndroidError::Sha1UpdateError;
        }

        return n;
    }
}

oc::result<void> LokiFormatWriter::finish_entry(File &file)
{
    OUTCOME_TRYV(m_seg->finish_entry(file));

    auto swentry = m_seg->entry();

    // Update SHA1 hash
    uint32_t le32_size = mb_htole32(*swentry->size);

    // Include fake 0 size for unsupported secondboot image
    if (swentry->type == EntryType::DeviceTree
            && !SHA1_Update(&m_sha_ctx, "\x00\x00\x00\x00", 4)) {
        return android::AndroidError::Sha1UpdateError;
    }

    // Include size for everything except empty DT images
    if (swentry->type != EntryType::Aboot
            && (swentry->type != EntryType::DeviceTree || *swentry->size > 0)
            && !SHA1_Update(&m_sha_ctx, &le32_size, sizeof(le32_size))) {
        return android::AndroidError::Sha1UpdateError;
    }

    switch (swentry->type) {
    case EntryType::Kernel:
        m_hdr.kernel_size = *swentry->size;
        break;
    case EntryType::Ramdisk:
        m_hdr.ramdisk_size = *swentry->size;
        break;
    case EntryType::DeviceTree:
        m_hdr.dt_size = *swentry->size;
        break;
    default:
        break;
    }

    return oc::success();
}

}
