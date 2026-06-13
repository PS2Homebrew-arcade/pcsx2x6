#include "ACATA_IO_CHD.h"
#include "common/StringUtil.h"
#include "common/Console.h"
#include <cstring>

ChdImage::ChdImage() = default;

ChdImage::~ChdImage()
{
    Close();
}

bool ChdImage::Open(const std::string& path)
{
    Close();

    chd_error err = chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &m_chd);

    if (err != CHDERR_NONE) {
        Console.ErrorFmt("{} failed to open CHD: {}", __FUNCTION__, (int)err);
        return false;
    }

    const chd_header* hdr = chd_get_header(m_chd);

    m_hunkSize = hdr->hunkbytes;
    m_unitBytes = hdr->unitbytes;
    m_totalUnits = hdr->logicalbytes / hdr->unitbytes;
    m_hunkBuffer.resize(m_hunkSize);

    // Work out the layout once: HDD/DVD units are already the logical sector; CD units are
    // raw frames whose 2048-byte payload sits at a track-dependent offset (read from metadata).
    if (m_unitBytes == 2352 || m_unitBytes == 2448)
    {
        m_type = ACMEDIATYPE::ACCD;
        m_logicalSize = 2048;
        m_frameDataOffset = DetectCdDataOffset();
    }
    else
    {
        m_type = (m_unitBytes == 2048) ? ACMEDIATYPE::ACDVD
               : (m_unitBytes == 512)  ? ACMEDIATYPE::ACHDD
               :                         ACMEDIATYPE::ACUNK;
        m_logicalSize = m_unitBytes;
        m_frameDataOffset = 0;
    }

    DevCon.WriteLnFmt("{}: opened ok (unit {}, logical {}, data offset {})",
                      __FUNCTION__, m_unitBytes, m_logicalSize, m_frameDataOffset);
    return true;
}

void ChdImage::Close()
{
    if (m_chd)
    {
        chd_close(m_chd);
        m_chd = nullptr;
    }

    m_hunkBuffer.clear();

    m_cachedHunk = UINT32_MAX;

    m_hunkSize = 0;
    m_unitBytes = 0;
    m_logicalSize = 0;
    m_frameDataOffset = 0;
    m_totalUnits = 0;

    m_type = ACMEDIATYPE::ACUNK;
}

bool ChdImage::IsOpen() const
{
    return m_chd != nullptr;
}

ACMEDIATYPE ChdImage::GetType() const
{
    return m_type;
}

u32 ChdImage::GetSectorSize() const
{
    return m_logicalSize;
}

u64 ChdImage::GetSectorCount() const
{
    return m_totalUnits;
}

// The CHD's metadata names the CD track format; map it to where the 2048-byte payload
// starts (raw modes prepend a sync + header; cooked modes keep it at offset 0).
u32 ChdImage::DetectCdDataOffset()
{
    char meta[256] = {};
    u32 len = 0;
    if (chd_get_metadata(m_chd, CDROM_TRACK_METADATA2_TAG, 0,
                         meta, sizeof(meta), &len, nullptr, nullptr) != CHDERR_NONE &&
        chd_get_metadata(m_chd, CDROM_TRACK_METADATA_TAG, 0,
                         meta, sizeof(meta), &len, nullptr, nullptr) != CHDERR_NONE)
        return 0;

    // Match the track type (" TYPE:"), not the pregap type ("PGTYPE:").
    if (std::strstr(meta, " TYPE:MODE2_RAW"))
        return 24;
    if (std::strstr(meta, " TYPE:MODE1_RAW"))
        return 16;
    return 0;
}

bool ChdImage::ReadHunk(u32 hunk)
{
    if (hunk == m_cachedHunk)
        return true;

    chd_error err =
        chd_read(m_chd,
                 hunk,
                 m_hunkBuffer.data());

    if (err != CHDERR_NONE)
        return false;

    m_cachedHunk = hunk;
    return true;
}

bool ChdImage::ReadSector(u64 lba, void* buffer)
{
    if (!m_chd)
        return false;

    if (lba >= m_totalUnits)
        return false;

    const u64 byteOffset = lba * m_unitBytes;

    const u32 hunk =
        static_cast<u32>(byteOffset / m_hunkSize);

    const u32 offset =
        static_cast<u32>(byteOffset % m_hunkSize);

    if ((offset + m_unitBytes) > m_hunkSize)
        return false;

    if (!ReadHunk(hunk))
        return false;

    std::memcpy(buffer, m_hunkBuffer.data() + offset + m_frameDataOffset, m_logicalSize);

    return true;
}

bool ChdImage::ReadSectors(u64 lba,
                           u32 count,
                           void* buffer)
{
    u8* dst = static_cast<u8*>(buffer);

    for (u32 i = 0; i < count; i++)
    {
        if (!ReadSector(lba + i,
                        dst + (i * m_logicalSize)))
        {
            return false;
        }
    }

    return true;
}

bool ChdImage::IsChdFileName(const std::string& path)
{
	return StringUtil::compareNoCase(Path::GetExtension(path), "chd");
}
