use std::{
    collections::{BTreeMap, BTreeSet},
    env, fs,
    fs::File,
    io::{Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    process::{self, Command, Stdio},
    time::{SystemTime, UNIX_EPOCH},
};

// S1a: representation-only Block-IR model + extractor (src/block_ir.rs).
// Not wired into translation or emission; see the module docs.
mod block_ir;
// S1b: Block-IR -> native C emitter for ONE straight-line block, behind the
// `--s1-block` generation flag + the DQ8_S1_BLOCK runtime gate.
mod block_emit;

const SECTOR: u64 = 0x200;

fn read_at(file: &mut File, offset: u64, len: usize) -> Result<Vec<u8>, String> {
    file.seek(SeekFrom::Start(offset))
        .map_err(|e| e.to_string())?;
    let mut data = vec![0; len];
    file.read_exact(&mut data)
        .map_err(|_| format!("unexpected end of file at 0x{offset:X}"))?;
    Ok(data)
}

fn u32le(b: &[u8], at: usize) -> Result<u32, String> {
    let s = b.get(at..at + 4).ok_or("short field")?;
    Ok(u32::from_le_bytes(s.try_into().unwrap()))
}

fn u64le(b: &[u8], at: usize) -> Result<u64, String> {
    let s = b.get(at..at + 8).ok_or("short field")?;
    Ok(u64::from_le_bytes(s.try_into().unwrap()))
}

fn sarc_u16(data: &[u8], at: usize, little: bool) -> Result<u16, String> {
    let s = data.get(at..at + 2).ok_or("truncated SARC field")?;
    Ok(if little {
        u16::from_le_bytes(s.try_into().unwrap())
    } else {
        u16::from_be_bytes(s.try_into().unwrap())
    })
}

fn sarc_u32(data: &[u8], at: usize, little: bool) -> Result<u32, String> {
    let s = data.get(at..at + 4).ok_or("truncated SARC field")?;
    Ok(if little {
        u32::from_le_bytes(s.try_into().unwrap())
    } else {
        u32::from_be_bytes(s.try_into().unwrap())
    })
}

fn extract_sarc(input: &Path, output_dir: &Path) -> Result<usize, String> {
    let data = fs::read(input).map_err(|e| format!("{}: {e}", input.display()))?;
    if data.get(..4) != Some(b"SARC") {
        return Err("input does not have a SARC header".into());
    }
    let little = match data.get(6..8) {
        Some([0xFF, 0xFE]) => true,
        Some([0xFE, 0xFF]) => false,
        _ => return Err("invalid SARC byte-order marker".into()),
    };
    let header_size = sarc_u16(&data, 4, little)? as usize;
    let file_size = sarc_u32(&data, 8, little)? as usize;
    let data_offset = sarc_u32(&data, 12, little)? as usize;
    if header_size < 0x14 || file_size > data.len() || file_size < header_size {
        return Err("invalid SARC size fields".into());
    }
    let sfat = header_size;
    if data.get(sfat..sfat + 4) != Some(b"SFAT") {
        return Err("SARC is missing its SFAT table".into());
    }
    let sfat_size = sarc_u16(&data, sfat + 4, little)? as usize;
    let count = sarc_u16(&data, sfat + 6, little)? as usize;
    if sfat_size < 0x0C {
        return Err("invalid SFAT header size".into());
    }
    let nodes = sfat.checked_add(sfat_size).ok_or("SFAT offset overflow")?;
    let nodes_end = nodes
        .checked_add(count.checked_mul(16).ok_or("SFAT count overflow")?)
        .ok_or("SFAT range overflow")?;
    if nodes_end > file_size || data.get(nodes_end..nodes_end + 4) != Some(b"SFNT") {
        return Err("invalid SARC node table or missing SFNT".into());
    }
    let sfnt_size = sarc_u16(&data, nodes_end + 4, little)? as usize;
    if sfnt_size < 8 {
        return Err("invalid SFNT header size".into());
    }
    let names = nodes_end
        .checked_add(sfnt_size)
        .ok_or("SFNT offset overflow")?;
    let mut written = 0;
    for i in 0..count {
        let node = nodes + i * 16;
        let attributes = sarc_u32(&data, node + 4, little)?;
        let start = sarc_u32(&data, node + 8, little)? as usize;
        let end = sarc_u32(&data, node + 12, little)? as usize;
        if attributes >> 24 == 0 {
            return Err(format!("SARC node {i} has no filename"));
        }
        let name_at = names
            .checked_add(
                ((attributes & 0x00FF_FFFF) as usize)
                    .checked_mul(4)
                    .ok_or("filename offset overflow")?,
            )
            .ok_or("filename offset overflow")?;
        if name_at >= data_offset {
            return Err(format!("SARC node {i} filename is out of range"));
        }
        let name_end = data[name_at..data_offset]
            .iter()
            .position(|b| *b == 0)
            .map(|n| name_at + n)
            .ok_or_else(|| format!("SARC node {i} filename is not terminated"))?;
        let name = std::str::from_utf8(&data[name_at..name_end])
            .map_err(|_| format!("SARC node {i} filename is not UTF-8"))?;
        let relative = Path::new(name);
        if relative.is_absolute()
            || relative
                .components()
                .any(|c| !matches!(c, std::path::Component::Normal(_)))
        {
            return Err(format!("unsafe SARC filename: {name}"));
        }
        let payload_start = data_offset
            .checked_add(start)
            .ok_or("payload offset overflow")?;
        let payload_end = data_offset
            .checked_add(end)
            .ok_or("payload offset overflow")?;
        if start > end || payload_end > file_size || payload_start > payload_end {
            return Err(format!("SARC node {i} payload is out of range"));
        }
        let destination = output_dir.join(relative);
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
        }
        fs::write(&destination, &data[payload_start..payload_end])
            .map_err(|e| format!("{}: {e}", destination.display()))?;
        written += 1;
    }
    Ok(written)
}

fn inspect_bflyt(path: &Path) -> Result<(), String> {
    let data = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if data.get(..4) != Some(b"FLYT") {
        return Err("input does not have a FLYT header".into());
    }
    let little = match data.get(4..6) {
        Some([0xFF, 0xFE]) => true,
        Some([0xFE, 0xFF]) => false,
        _ => return Err("invalid FLYT byte-order marker".into()),
    };
    let header_size = sarc_u16(&data, 6, little)? as usize;
    let section_count = sarc_u16(&data, 0x10, little)? as usize;
    let file_size = sarc_u32(&data, 0x0C, little)? as usize;
    if header_size < 0x14 || file_size > data.len() || file_size < header_size {
        return Err("invalid FLYT size fields".into());
    }
    println!("Layout: {}", path.display());
    println!("Sections: {section_count}");
    let mut offset = header_size;
    for _ in 0..section_count {
        let tag = data
            .get(offset..offset + 4)
            .ok_or("truncated FLYT section header")?;
        let size = sarc_u32(&data, offset + 4, little)? as usize;
        if size < 8 || offset.checked_add(size).is_none_or(|end| end > file_size) {
            return Err(format!("invalid FLYT section at 0x{offset:X}"));
        }
        match tag {
            b"lyt1" if size >= 24 => {
                let width = f32::from_bits(sarc_u32(&data, offset + 12, little)?);
                let height = f32::from_bits(sarc_u32(&data, offset + 16, little)?);
                println!("Canvas: {width} x {height}");
            }
            b"txl1" if size >= 12 => {
                let count = sarc_u32(&data, offset + 8, little)? as usize;
                println!("Textures ({count}):");
                for i in 0..count {
                    let string_offset = sarc_u32(&data, offset + 12 + i * 4, little)? as usize;
                    let name_at = offset
                        .checked_add(12)
                        .and_then(|base| base.checked_add(string_offset))
                        .ok_or("texture name offset overflow")?;
                    let limit = offset + size;
                    if name_at >= limit {
                        return Err("texture name offset is out of range".into());
                    }
                    let end = data[name_at..limit]
                        .iter()
                        .position(|b| *b == 0)
                        .map(|n| name_at + n)
                        .ok_or("unterminated texture name")?;
                    println!("  {}", String::from_utf8_lossy(&data[name_at..end]));
                }
            }
            b"pan1" | b"pic1" | b"wnd1" if size >= 84 => {
                let name_end = data[offset + 12..offset + 36]
                    .iter()
                    .position(|b| *b == 0)
                    .map(|n| offset + 12 + n)
                    .unwrap_or(offset + 36);
                let name = String::from_utf8_lossy(&data[offset + 12..name_end]);
                let x = f32::from_bits(sarc_u32(&data, offset + 40, little)?);
                let y = f32::from_bits(sarc_u32(&data, offset + 44, little)?);
                let width = f32::from_bits(sarc_u32(&data, offset + 76, little)?);
                let height = f32::from_bits(sarc_u32(&data, offset + 80, little)?);
                println!(
                    "Node {} {}: at ({x:.1}, {y:.1}), size {width:.1} x {height:.1}",
                    String::from_utf8_lossy(tag),
                    name
                );
            }
            _ => {}
        }
        offset += size;
    }
    if offset != file_size {
        return Err("FLYT section count does not match file size".into());
    }
    Ok(())
}

fn inspect_ncch(f: &mut File, start: u64, part_size: u64, index: usize) -> Result<(), String> {
    let h = read_at(f, start, 0x200)?;
    let program_id = u64le(&h, 0x118)?;
    let product = String::from_utf8_lossy(&h[0x150..0x160])
        .trim_matches('\0')
        .to_string();
    let exheader_size = u32le(&h, 0x180)? as usize;
    let flags = &h[0x188..0x190];
    let no_crypto = flags[7] & 1 != 0;
    println!("      NCCH program_id=0x{program_id:016X} product={product}");
    println!(
        "      exheader_size=0x{exheader_size:X} flags={:02x?}",
        flags
    );
    println!(
        "      content_crypto={}",
        if no_crypto {
            "disabled (NoCrypto)"
        } else {
            "enabled"
        }
    );

    if exheader_size >= 0x1C && 0x200 + exheader_size as u64 <= part_size {
        let exh = read_at(f, start + 0x200, exheader_size.min(0x400))?;
        let name = String::from_utf8_lossy(&exh[..8])
            .trim_matches('\0')
            .to_string();
        let load = u32le(&exh, 0x10)?;
        let pages = u32le(&exh, 0x14)?;
        let code_size = u32le(&exh, 0x18)?;
        let compressed = exh[0x0D] & 1 != 0;
        if !name.is_empty() && load != 0 && pages != 0 {
            println!(
                "      exheader name={name} code_load=0x{load:08X} code_pages=0x{pages:X} code_size=0x{code_size:X}"
            );
            println!(
                "      exheader code_compressed={}",
                if compressed { "yes" } else { "no" }
            );
        }
    }

    let mut sections = Vec::new();
    for (name, field) in [
        ("plain", 0x190),
        ("logo", 0x198),
        ("exefs", 0x1A0),
        ("romfs", 0x1B0),
    ] {
        let off = u32le(&h, field)? as u64 * SECTOR;
        let size = u32le(&h, field + 4)? as u64 * SECTOR;
        let hash = u32le(&h, field + 8)? as u64 * SECTOR;
        if off == 0 || size == 0 {
            continue;
        }
        let valid = off.checked_add(size).is_some_and(|end| end <= part_size);
        println!(
            "      {name:5} offset=0x{off:08X} size=0x{size:08X} hash_region=0x{hash:X}{}",
            if valid { "" } else { " INVALID" }
        );
        if name == "exefs" {
            sections.push((off, size, valid));
        }
    }
    let Some((exefs_off, exefs_size, true)) = sections.first().copied() else {
        return Ok(());
    };
    if exefs_size < 0x200 {
        return Ok(());
    }
    let table = read_at(f, start + exefs_off, 0x200)?;
    println!("      ExeFS files:");
    for slot in 0..10 {
        let at = slot * 0x10;
        let name_bytes = &table[at..at + 8];
        let end = name_bytes.iter().position(|&b| b == 0).unwrap_or(8);
        if end == 0 {
            continue;
        }
        let name = String::from_utf8_lossy(&name_bytes[..end]);
        let off = u32le(&table, at + 8)? as u64;
        let size = u32le(&table, at + 12)? as u64;
        let absolute = start + exefs_off + 0x200 + off;
        let valid = off
            .checked_add(size)
            .is_some_and(|e| e <= exefs_size.saturating_sub(0x200));
        println!(
            "        {name:8} offset=0x{off:08X} size=0x{size:08X} image_offset=0x{absolute:08X}{}",
            if valid { "" } else { " INVALID" }
        );
        if name == ".code" && valid && size != 0 {
            let sample = read_at(f, absolute, size.min(16) as usize)?;
            println!(
                "          state={}; first_bytes={:02x?}",
                if no_crypto {
                    "plaintext section"
                } else {
                    "encrypted content section; not ARM instructions until decrypted"
                },
                sample
            );
        }
    }
    let _ = index;
    Ok(())
}

fn inspect(path: &Path) -> Result<(), String> {
    let mut f = File::open(path).map_err(|e| e.to_string())?;
    let file_size = f.metadata().map_err(|e| e.to_string())?.len();
    let hdr = read_at(&mut f, 0x100, 0x100)?;
    if &hdr[..4] != b"NCSD" {
        return Err("NCSD magic not found at offset 0x100".into());
    }
    let media_size = u32le(&hdr, 4)? as u64 * SECTOR;
    println!("Image:       {}", path.display());
    println!("File size:   {file_size} bytes");
    println!("NCSD size:   {media_size} bytes");
    println!("Partitions:");
    for i in 0..8 {
        let at = 0x20 + i * 8;
        let off = u32le(&hdr, at)? as u64 * SECTOR;
        let size = u32le(&hdr, at + 4)? as u64 * SECTOR;
        if off == 0 || size == 0 {
            continue;
        }
        if off.checked_add(size).is_none_or(|e| e > file_size) {
            println!("  [{i}] invalid range offset=0x{off:X} size=0x{size:X} (past EOF)");
            continue;
        }
        let magic = read_at(&mut f, off + 0x100, 4)?;
        let magic_str = String::from_utf8_lossy(&magic);
        println!("  [{i}] offset=0x{off:08X} size=0x{size:08X} header={magic_str}");
        if &magic == b"NCCH" {
            inspect_ncch(&mut f, off, size, i)?;
        }
    }
    println!("\nContainer metadata only; encrypted content is not decrypted.");
    Ok(())
}

fn blz_decompress(data: &[u8]) -> Result<Vec<u8>, String> {
    if data.len() < 8 {
        return Err("input too short for BLZ footer".into());
    }
    let packed = u32le(data, data.len() - 8)?;
    let extra = u32le(data, data.len() - 4)? as usize;
    let header_size = (packed >> 24) as usize;
    let compressed_size = (packed & 0x00FF_FFFF) as usize;
    if extra == 0 {
        return Ok(data.to_vec());
    }
    if header_size < 8 || header_size > data.len() {
        return Err("invalid BLZ header size".into());
    }
    if compressed_size < header_size || compressed_size > data.len() {
        return Err("invalid BLZ compressed size".into());
    }
    if data[data.len() - header_size..data.len() - 8]
        .iter()
        .any(|&b| b != 0xFF)
    {
        return Err("BLZ footer padding is invalid".into());
    }
    let prefix_size = data.len() - compressed_size;
    let output_size = data
        .len()
        .checked_add(extra)
        .ok_or("BLZ output size overflow")?;
    if output_size > 512 * 1024 * 1024 {
        return Err("BLZ output exceeds 512 MiB safety limit".into());
    }
    let body_end = prefix_size + compressed_size - header_size;
    let body = &data[prefix_size..body_end];
    let body_output_size = output_size - prefix_size;
    let mut out = Vec::with_capacity(body_output_size);
    let (mut cursor, mut flags, mut mask) = (0usize, 0u8, 1u8);
    while out.len() < body_output_size {
        if mask == 1 {
            if cursor >= body.len() {
                return Err("BLZ stream ended while reading flags".into());
            }
            flags = body[body.len() - 1 - cursor];
            cursor += 1;
            mask = 0x80;
        } else {
            mask >>= 1;
        }
        if flags & mask != 0 {
            if cursor + 1 >= body.len() {
                return Err("BLZ stream ended in a back-reference".into());
            }
            let a = body[body.len() - 1 - cursor];
            let b = body[body.len() - 2 - cursor];
            cursor += 2;
            let count = (a >> 4) as usize + 3;
            let mut disp = ((((a & 0x0F) as usize) << 8) | b as usize) + 3;
            if disp > out.len() {
                if out.len() < 2 {
                    return Err("BLZ back-reference precedes decoded data".into());
                }
                disp = 2;
            }
            let mut src = out.len() - disp;
            for _ in 0..count {
                if out.len() == body_output_size {
                    break;
                }
                let value = *out.get(src).ok_or("invalid BLZ back-reference")?;
                out.push(value);
                src += 1;
            }
        } else {
            if cursor >= body.len() {
                return Err("BLZ stream ended while reading a literal".into());
            }
            out.push(body[body.len() - 1 - cursor]);
            cursor += 1;
        }
    }
    out.reverse();
    let mut result = data[..prefix_size].to_vec();
    result.extend(out);
    Ok(result)
}

fn extract_code(
    image: &Path,
    output: &Path,
    partition: usize,
    decompress: bool,
) -> Result<(), String> {
    if partition > 7 {
        return Err("partition must be 0 through 7".into());
    }
    let mut f = File::open(image).map_err(|e| e.to_string())?;
    let hdr = read_at(&mut f, 0x100, 0x100)?;
    if &hdr[..4] != b"NCSD" {
        return Err("NCSD magic not found at offset 0x100".into());
    }
    let part_off = u32le(&hdr, 0x20 + partition * 8)? as u64 * SECTOR;
    let part_size = u32le(&hdr, 0x24 + partition * 8)? as u64 * SECTOR;
    if part_off == 0 || part_size == 0 {
        return Err(format!("partition {partition} is empty"));
    }
    let h = read_at(&mut f, part_off, 0x200)?;
    if &h[0x100..0x104] != b"NCCH" {
        return Err(format!("partition {partition} has no NCCH header"));
    }
    if h[0x18F] & 1 == 0 {
        return Err("NCCH content is encrypted; provide a decrypted .code file to disasm".into());
    }
    let exheader_size = u32le(&h, 0x180)? as usize;
    if exheader_size < 0x1C || 0x200 + exheader_size as u64 > part_size {
        return Err("no usable NCCH extended header".into());
    }
    let exh = read_at(&mut f, part_off + 0x200, exheader_size.min(0x400))?;
    let load = u32le(&exh, 0x10)?;
    let pages = u32le(&exh, 0x14)?;
    let code_size = u32le(&exh, 0x18)?;
    let compressed = exh[0x0D] & 1 != 0;
    let exefs_off = u32le(&h, 0x1A0)? as u64 * SECTOR;
    let exefs_size = u32le(&h, 0x1A4)? as u64 * SECTOR;
    if exefs_size < 0x200 || exefs_off + exefs_size > part_size {
        return Err("invalid ExeFS range".into());
    }
    let table = read_at(&mut f, part_off + exefs_off, 0x200)?;
    let mut code_entry = None;
    for slot in 0..10 {
        let at = slot * 0x10;
        if table[at..at + 8].split(|&b| b == 0).next() == Some(b".code") {
            code_entry = Some((
                u32le(&table, at + 8)? as u64,
                u32le(&table, at + 12)? as u64,
            ));
            break;
        }
    }
    let (code_off, code_len) = code_entry.ok_or("ExeFS has no .code entry")?;
    if code_off + code_len > exefs_size - 0x200 {
        return Err(".code range exceeds ExeFS".into());
    }
    let mut code = read_at(
        &mut f,
        part_off + exefs_off + 0x200 + code_off,
        code_len as usize,
    )?;
    if decompress && compressed {
        code = blz_decompress(&code)?;
    }
    let mut out = File::create(output).map_err(|e| e.to_string())?;
    out.write_all(&code).map_err(|e| e.to_string())?;
    println!("Wrote {} bytes to {}", code.len(), output.display());
    println!("Load address: 0x{load:08X}; header code size: 0x{code_size:X}; pages: 0x{pages:X}");
    println!(
        "BLZ decompression: {}",
        if decompress && compressed {
            "applied"
        } else {
            "not requested or not flagged"
        }
    );
    Ok(())
}

fn decrypt_code_with_ctrtool(
    image: &Path,
    output: &Path,
    partition: usize,
    ctrtool: &str,
    keyset: Option<&Path>,
) -> Result<(), String> {
    if partition > 7 {
        return Err("partition must be 0 through 7".into());
    }
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let temp_dir = env::temp_dir().join(format!("dq8tool-{}-{nonce}", process::id()));
    fs::create_dir(&temp_dir).map_err(|e| format!("cannot create temporary directory: {e}"))?;
    let exefs_dir = temp_dir.join("exefs");
    let mut command = Command::new(ctrtool);
    command
        .arg(format!("--exefsdir={}", exefs_dir.display()))
        .arg("--ncch")
        .arg(partition.to_string());
    if let Some(keys) = keyset {
        command.arg(format!("--keyset={}", keys.display()));
    }
    let result = command
        .arg(image)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map_err(|e| format!("could not run {ctrtool}: {e}"))
        .and_then(|status| {
            if !status.success() {
                return Err(format!("ctrtool failed with status {status}"));
            }
            let code = exefs_dir.join("code.bin");
            let meta = fs::metadata(&code)
                .map_err(|e| format!("ctrtool succeeded but did not create ExeFS .code: {e}"))?;
            if meta.len() < 4 {
                return Err("ctrtool produced an unexpectedly small .code file".into());
            }
            if let Some(parent) = output.parent().filter(|p| !p.as_os_str().is_empty()) {
                fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            fs::copy(&code, output).map_err(|e| format!("cannot save decrypted .code: {e}"))?;
            println!(
                "Wrote {} decrypted/decompressed .code bytes to {}",
                meta.len(),
                output.display()
            );
            Ok(())
        });
    let _ = fs::remove_dir_all(&temp_dir);
    result
}

const REGS: [&str; 16] = [
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "ip", "sp", "lr",
    "pc",
];
const COND: [&str; 16] = [
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "", "",
];

fn decode_arm(w: u32, addr: u32) -> String {
    let cond = COND[(w >> 28) as usize];
    let suffix = if cond.is_empty() {
        String::new()
    } else {
        format!(".{cond}")
    };
    // ARMv5TE PLD uses the single-data-transfer encoding space, with Rd=pc
    // as part of the fixed opcode. It is a hint, not a load-to-PC branch.
    if w & 0xFD70_F000 == 0xF550_F000 {
        let rn = REGS[((w >> 16) & 15) as usize];
        let offset = w & 0xFFF;
        let sign = if w & (1 << 23) != 0 { "+" } else { "-" };
        return format!("prefetch [{rn}{sign}0x{offset:X}];");
    }
    if w == 0xF57F_F01F {
        return "clrex;".to_string();
    }
    if w & 0xFFFF_FFFF == 0xF57F_F04F {
        return "dsb;".to_string();
    }
    if w == 0xF57F_F05F {
        return "dmb;".to_string();
    }
    if w == 0xF57F_F06F {
        return "isb;".to_string();
    }
    // ARMv6 NOP is encoded in the data-processing space like TEQ, but it is
    // a hint and must leave the condition flags untouched. DQ8 uses it between
    // SUBS and BNE in a counted copy loop.
    if w == 0xE320_F000 {
        return "nop;".to_string();
    }
    if w & 0x0E00_0000 == 0x0A00_0000 {
        let delta = (((w & 0x00FF_FFFF) << 8) as i32 >> 6) as u32;
        return format!(
            "{}{}(0x{:08X});",
            if w & (1 << 24) != 0 { "call" } else { "goto" },
            suffix,
            addr.wrapping_add(8).wrapping_add(delta)
        );
    }
    if w & 0x0FFF_FFF0 == 0x012F_FF10 {
        return format!("branch{}({});", suffix, REGS[(w & 15) as usize]);
    }
    if w & 0x0E00_0000 == 0x0800_0000 {
        let load = w & (1 << 20) != 0;
        let base = REGS[((w >> 16) & 15) as usize];
        let list = w & 0xFFFF;
        let regs = (0..16)
            .filter(|r| list & (1 << r) != 0)
            .map(|r| REGS[r])
            .collect::<Vec<_>>()
            .join(", ");
        let direction = match (w >> 23) & 3 {
            0 => "da",
            1 => "ia",
            2 => "db",
            _ => "ib",
        };
        let suffix = if w & (1 << 21) != 0 { "!" } else { "" };
        let instruction = format!(
            "{}{} {base}{suffix}, {{{regs}}};",
            if load { "ldm" } else { "stm" },
            direction
        );
        return if cond.is_empty() {
            instruction
        } else {
            format!("if ({cond}) {instruction}")
        };
    }
    if matches!(w & 0x0FE0_0030, 0x06A0_0010 | 0x06E0_0010) {
        let signed = w & 0x0040_0000 == 0;
        let sat_field = (w >> 16) & 31;
        let sat = if signed { sat_field + 1 } else { sat_field };
        let shift = (w >> 7) & 31;
        let shift_text = if shift == 0 && w & (1 << 6) == 0 {
            String::new()
        } else if w & (1 << 6) != 0 {
            format!(", asr #{}", if shift == 0 { 32 } else { shift })
        } else {
            format!(", lsl #{shift}")
        };
        let instruction = format!(
            "{} {}, #{sat}, {}{};",
            if signed { "ssat" } else { "usat" },
            REGS[((w >> 12) & 15) as usize],
            REGS[(w & 15) as usize],
            shift_text
        );
        return if cond.is_empty() {
            instruction
        } else {
            format!("if ({cond}) {instruction}")
        };
    }
    if matches!(w & 0x0FF0_0FF0, 0x06A0_0F30 | 0x06E0_0F30) {
        let signed = w & 0x0040_0000 == 0;
        let instruction = format!(
            "{}16 {}, #{}, {};",
            if signed { "ssat" } else { "usat" },
            REGS[((w >> 12) & 15) as usize],
            ((w >> 16) & 15) + u32::from(signed),
            REGS[(w & 15) as usize]
        );
        return if cond.is_empty() {
            instruction
        } else {
            format!("if ({cond}) {instruction}")
        };
    }
    if w & 0x0C00_0000 == 0x0400_0000 {
        let load = w & (1 << 20) != 0;
        let up = w & (1 << 23) != 0;
        let base = REGS[((w >> 16) & 15) as usize];
        let rd = REGS[((w >> 12) & 15) as usize];
        let pre = w & (1 << 24) != 0;
        let off = if w & (1 << 25) != 0 {
            format!("{} , shift={}", REGS[(w & 15) as usize], (w >> 4) & 0xFF)
        } else {
            format!("0x{:X}", w & 0xFFF)
        };
        let sign = if up { "+" } else { "-" };
        let ea = if off == "0x0" {
            format!("{base}")
        } else {
            format!("{base}{sign}({off})")
        };
        let ea = if pre { ea } else { base.to_string() };
        let wb = if w & (1 << 21) != 0 { "!" } else { "" };
        let byte = if w & (1 << 22) != 0 { "u8 " } else { "" };
        let mut instruction = if load {
            format!("{rd} = *({byte}{ea}){wb};")
        } else {
            format!("*({byte}{ea}){wb} = {rd};")
        };
        if !pre || w & (1 << 21) != 0 {
            instruction.push_str(&format!(" {base} {sign}= {off};"));
        }
        return if cond.is_empty() {
            instruction
        } else {
            format!("if ({cond}) {instruction}")
        };
    }
    if w & 0x0C00_0000 == 0 {
        let op = (w >> 21) & 15;
        let rd = REGS[((w >> 12) & 15) as usize];
        let rn = REGS[((w >> 16) & 15) as usize];
        let operand = if w & (1 << 25) != 0 {
            let imm = w & 0xFF;
            let rot = ((w >> 8) & 15) * 2;
            format!("0x{:X}", imm.rotate_right(rot))
        } else {
            REGS[(w & 15) as usize].to_string()
        };
        let expr = match op {
            0 => Some(format!("{rd} = {rn} & {operand};")),
            1 => Some(format!("{rd} = {rn} ^ {operand};")),
            2 => Some(format!("{rd} = {rn} - {operand};")),
            3 => Some(format!("{rd} = {operand} - {rn};")),
            4 => Some(format!("{rd} = {rn} + {operand};")),
            5 => Some(format!("{rd} = {rn} + {operand} + carry;")),
            6 => Some(format!("{rd} = {rn} - {operand} - !carry;")),
            7 => Some(format!("{rd} = {operand} - {rn} - !carry;")),
            8 => Some(format!("test({rn}, {operand});")),
            9 => Some(format!("test({rn}, ~{operand});")),
            10 => Some(format!("compare({rn}, {operand});")),
            11 => Some(format!("compare({rn}, {operand} + carry);")),
            12 => Some(format!("{rd} = {rn} | {operand};")),
            13 => Some(format!("{rd} = {operand};")),
            14 => Some(format!("{rd} = {rn} & ~{operand};")),
            15 => Some(format!("{rd} = ~{operand};")),
            _ => None,
        };
        if let Some(s) = expr {
            return if cond.is_empty() {
                s
            } else {
                format!("if ({cond}) {s}")
            };
        }
    }
    format!("/* unsupported ARM 0x{w:08X} */")
}

fn arm_branch_target(w: u32, addr: u32) -> Option<(u32, bool, bool)> {
    if w & 0x0E00_0000 != 0x0A00_0000 {
        return None;
    }
    let blx = w >> 28 == 0xF;
    let mut delta = (((w & 0x00FF_FFFF) << 8) as i32 >> 6) as u32;
    if blx && w & (1 << 24) != 0 {
        delta = delta.wrapping_add(2);
    }
    Some((
        addr.wrapping_add(8).wrapping_add(delta),
        w & (1 << 24) != 0 || blx,
        w >> 28 != 0xE && !blx,
    ))
}

fn arm_pc_relative_ldr_target(data: &[u8], base: u32, addr: u32, w: u32) -> Option<u32> {
    // LDR pc, [pc, +/-imm] with pre-indexing is a statically readable
    // veneer/jump-table entry. Register offsets and post-indexed forms need
    // runtime state and are left unresolved.
    // Bit 25 is the immediate/register selector: a shifted-register form such
    // as `ldrlo pc, [pc, r0, lsl #2]` must NOT be treated as an immediate
    // load - doing so resolves a bogus "literal" (here the word of a
    // different `ldr rN, =0x005443BC` pool entry) and enqueues a data
    // address as a function entry. Register-offset switches are handled by
    // arm_pc_relative_jump_table_targets instead.
    if w & 0x0C5F_F000 != 0x041F_F000 || w & (1 << 24) == 0 || w & (1 << 25) != 0 {
        return None;
    }
    let pc = addr.checked_add(8)?;
    let imm = w & 0xFFF;
    let literal = if w & (1 << 23) != 0 {
        pc.checked_add(imm)?
    } else {
        pc.checked_sub(imm)?
    };
    let offset = literal.checked_sub(base)? as usize;
    let bytes: [u8; 4] = data.get(offset..offset + 4)?.try_into().ok()?;
    Some(u32::from_le_bytes(bytes))
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
enum ArmMode {
    Arm,
    Thumb,
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
struct FuncKey {
    addr: u32,
    mode: ArmMode,
}

fn thumb_literal(data: &[u8], base: u32, addr: u32, insn: u16) -> Option<u32> {
    if insn & 0xF800 != 0x4800 {
        return None;
    }
    let literal = (addr.wrapping_add(4) & !3).wrapping_add(((insn & 0xFF) as u32) << 2);
    let off = literal.checked_sub(base)? as usize;
    let bytes = data.get(off..off + 4)?;
    Some(u32::from_le_bytes(bytes.try_into().ok()?))
}

fn discover_thumb_relptr_init_table(
    data: &[u8],
    base: u32,
    text_end: u32,
    entry: u32,
) -> BTreeSet<FuncKey> {
    let mut found = BTreeSet::new();
    let off = match entry.checked_sub(base) {
        Some(v) => v as usize,
        None => return found,
    };
    if off + 0x24 > data.len() {
        return found;
    }
    let h: Vec<u16> = (0..12)
        .map(|i| u16::from_le_bytes(data[off + i * 2..off + i * 2 + 2].try_into().unwrap()))
        .collect();
    // DQ8's Thumb init-array dispatcher loads start/end pointers, then iterates
    // signed offsets and calls each reconstructed pointer with BLX r0.
    if h[1] & 0xF800 != 0x4800
        || (h[1] >> 8) & 7 != 4
        || h[2] != 0x447C
        || h[3] & 0xF800 != 0x4800
        || (h[3] >> 8) & 7 != 5
        || h[4] != 0x447D
        || h[6] & 0xF800 != 0x6800
        || h[7] != 0x1900
        || h[8] != 0x4780
        || h[9] & 0xF800 != 0x1800
        || h[10] != 0x42AC
        || h[11] & 0xFF00 != 0xD100
    {
        return found;
    }
    let start = match thumb_literal(data, base, entry + 2, h[1]) {
        Some(v) => v.wrapping_add((entry + 8) & !3),
        None => return found,
    };
    let end = match thumb_literal(data, base, entry + 6, h[3]) {
        Some(v) => v.wrapping_add((entry + 12) & !3),
        None => return found,
    };
    if start < base || end <= start || end - start > 0x10000 {
        return found;
    }
    let mut cursor = start;
    while cursor + 4 <= end {
        let loc = match cursor.checked_sub(base) {
            Some(v) => v as usize,
            None => break,
        };
        let Some(bytes) = data.get(loc..loc + 4) else {
            break;
        };
        let relative = i32::from_le_bytes(bytes.try_into().unwrap());
        let target = cursor.wrapping_add(relative as u32);
        if target >= base && target < text_end {
            found.insert(FuncKey {
                addr: target & !1,
                mode: if target & 1 != 0 {
                    ArmMode::Thumb
                } else {
                    ArmMode::Arm
                },
            });
        }
        cursor += 4;
    }
    found
}

fn arm_restores_lr_before_bx(data: &[u8], offset: usize) -> bool {
    if offset < 4 {
        return false;
    }
    let prev = u32::from_le_bytes(data[offset - 4..offset].try_into().unwrap());
    prev & 0x0FFF_0000 == 0x08BD_0000
        && prev & (1 << 20) != 0
        && prev & (1 << 14) != 0
        && prev & (1 << 15) == 0
}

fn arm_pc_relative_bx_target(data: &[u8], offset: usize, addr: u32, rm: usize) -> Option<u32> {
    if offset < 4 {
        return None;
    }
    let prev = u32::from_le_bytes(data[offset - 4..offset].try_into().ok()?);
    if ((prev >> 12) & 15) as usize != rm {
        return None;
    }
    let op = prev & 0x0FEF_0000;
    if op != 0x028F_0000 && op != 0x024F_0000 {
        return None;
    }
    let imm = (prev & 0xFF).rotate_right(((prev >> 8) & 15) * 2);
    let pc = addr.wrapping_add(4);
    Some(if op == 0x024F_0000 {
        pc.wrapping_sub(imm)
    } else {
        pc.wrapping_add(imm)
    })
}

fn thumb_restores_stack_before_bx(data: &[u8], offset: usize) -> bool {
    let mut saw_pop = false;
    for back in (2..=8).step_by(2) {
        if offset < back {
            break;
        }
        let h = u16::from_le_bytes(data[offset - back..offset - back + 2].try_into().unwrap());
        if h & 0xFE00 == 0xBC00 {
            saw_pop = true;
        } else if h & 0xFF00 == 0xB000 && h & 0x0080 == 0 {
            // ADD SP, #imm restores the remaining stack frame after POP.
        } else if saw_pop {
            break;
        }
    }
    saw_pop
}

fn trace_function(
    data: &[u8],
    base: u32,
    text_end: u32,
    key: FuncKey,
    limit: usize,
) -> (BTreeMap<u32, String>, BTreeSet<FuncKey>) {
    let mut lines = BTreeMap::new();
    let mut calls = BTreeSet::new();
    if key.mode == ArmMode::Thumb {
        calls.extend(discover_thumb_relptr_init_table(
            data, base, text_end, key.addr,
        ));
    }
    let mut blocks = vec![key.addr];
    let mut visited = BTreeSet::new();
    let mut count = 0usize;
    while let Some(mut addr) = blocks.pop() {
        loop {
            let width = if key.mode == ArmMode::Arm { 4 } else { 2 };
            if addr < base
                || addr >= text_end
                || addr as usize + width > base as usize + data.len()
                || !visited.insert(addr)
                || count >= limit
            {
                break;
            }
            let offset = (addr - base) as usize;
            count += 1;
            match key.mode {
                ArmMode::Arm => {
                    let w = u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap());
                    if let Some((target, link, conditional)) = arm_branch_target(w, addr) {
                        let exchange = w >> 28 == 0xF;
                        if link {
                            if target >= base && target < text_end {
                                calls.insert(FuncKey {
                                    addr: target & !1,
                                    mode: if exchange {
                                        ArmMode::Thumb
                                    } else {
                                        ArmMode::Arm
                                    },
                                });
                            }
                            lines.insert(
                                addr,
                                format!(
                                    "{w:08X}  {}0x{target:08X}{}",
                                    if exchange { "blx-thumb " } else { "bl " },
                                    if conditional {
                                        format!(" if {}", COND[(w >> 28) as usize])
                                    } else {
                                        String::new()
                                    }
                                ),
                            );
                            addr = addr.wrapping_add(4);
                            continue;
                        }
                        lines.insert(
                            addr,
                            format!(
                                "{w:08X}  b{}0x{target:08X}",
                                if conditional {
                                    format!(".{} ", COND[(w >> 28) as usize])
                                } else {
                                    " ".into()
                                }
                            ),
                        );
                        if target >= base && target < text_end {
                            blocks.push(target);
                        }
                        if conditional {
                            addr = addr.wrapping_add(4);
                            continue;
                        }
                        break;
                    }
                    let text = decode_arm(w, addr);
                    if w & 0x0FFF_FFF0 == 0x012F_FF10 {
                        let rm = (w & 15) as usize;
                        let indirect = if rm == 14 {
                            "return".to_string()
                        } else if let Some(target) =
                            arm_pc_relative_bx_target(data, offset, addr, rm)
                        {
                            let mode = if target & 1 != 0 {
                                ArmMode::Thumb
                            } else {
                                ArmMode::Arm
                            };
                            let target = target & !1;
                            if target >= base && target < text_end {
                                calls.insert(FuncKey { addr: target, mode });
                            }
                            format!(
                                "branch_to_{}(0x{target:08X});",
                                if mode == ArmMode::Thumb {
                                    "thumb"
                                } else {
                                    "arm"
                                }
                            )
                        } else if arm_restores_lr_before_bx(data, offset) {
                            format!("tailcall_indirect({});", REGS[rm])
                        } else {
                            format!("branch_indirect({});", REGS[rm])
                        };
                        lines.insert(addr, format!("{w:08X}  {indirect}"));
                        if rm != 14 && offset >= 4 {
                            let prev =
                                u32::from_le_bytes(data[offset - 4..offset].try_into().unwrap());
                            let prev_addr = addr - 4;
                            if prev & 0x0FEF_0000 == 0x028F_0000
                                && ((prev >> 12) & 15) as usize == rm
                            {
                                let imm = (prev & 0xFF).rotate_right(((prev >> 8) & 15) * 2);
                                let target = prev_addr.wrapping_add(8).wrapping_add(imm);
                                let mode = if target & 1 != 0 {
                                    ArmMode::Thumb
                                } else {
                                    ArmMode::Arm
                                };
                                calls.insert(FuncKey {
                                    addr: target & !1,
                                    mode,
                                });
                            }
                        }
                        break;
                    }
                    let arm_hint_or_barrier = w & 0xFD70_F000 == 0xF550_F000
                        || matches!(w, 0xF57F_F01F | 0xF57F_F04F | 0xF57F_F05F | 0xF57F_F06F);
                    if !arm_hint_or_barrier
                        && w & 0x0C00_0000 == 0x0400_0000
                        && w & (1 << 20) != 0
                        && (w >> 12) & 15 == 15
                    {
                        let rn = (w >> 16) & 15;
                        if rn == 15 {
                            if let Some(target) = arm_pc_relative_ldr_target(data, base, addr, w) {
                                let mode = if target & 1 != 0 {
                                    ArmMode::Thumb
                                } else {
                                    ArmMode::Arm
                                };
                                let target_addr = target & !1;
                                lines.insert(
                                    addr,
                                    format!(
                                        "{w:08X}  branch_to_{}(0x{target_addr:08X});",
                                        if mode == ArmMode::Thumb {
                                            "thumb"
                                        } else {
                                            "arm"
                                        }
                                    ),
                                );
                                if target_addr >= base && target_addr < text_end {
                                    calls.insert(FuncKey {
                                        addr: target_addr,
                                        mode,
                                    });
                                }
                                if w >> 28 != 0xE {
                                    blocks.push(addr.wrapping_add(4));
                                }
                                break;
                            }
                        }
                        let branch = if rn == 13 {
                            "return_indirect;".to_string()
                        } else {
                            format!("branch_indirect({});", REGS[rn as usize])
                        };
                        lines.insert(addr, format!("{w:08X}  {branch}"));
                        if w >> 28 != 0xE {
                            blocks.push(addr.wrapping_add(4));
                        }
                        break;
                    }
                    if w & 0x0C00_0000 == 0
                        && (w >> 12) & 15 == 15
                        && !matches!((w >> 21) & 15, 8..=11)
                    {
                        let op = (w >> 21) & 15;
                        let rm = (w & 15) as usize;
                        let branch = if op == 13 && w & (1 << 25) == 0 && rm == 14 {
                            "return;".to_string()
                        } else {
                            format!("branch_indirect({});", REGS[rm])
                        };
                        lines.insert(addr, format!("{w:08X}  {branch}"));
                        if w >> 28 != 0xE {
                            blocks.push(addr.wrapping_add(4));
                        }
                        break;
                    }
                    lines.insert(addr, format!("{w:08X}  {text}"));
                    if w & 0x0E10_8000 == 0x0810_8000 {
                        break;
                    }
                    addr = addr.wrapping_add(4);
                }
                ArmMode::Thumb => {
                    let h = u16::from_le_bytes(data[offset..offset + 2].try_into().unwrap());
                    if h & 0xF800 == 0xF000 && offset + 4 <= data.len() {
                        let h2 =
                            u16::from_le_bytes(data[offset + 2..offset + 4].try_into().unwrap());
                        if let Some((target, to_arm)) = thumb_bl_target(addr, h, h2) {
                            calls.insert(FuncKey {
                                addr: target,
                                mode: if to_arm { ArmMode::Arm } else { ArmMode::Thumb },
                            });
                            lines.insert(
                                addr,
                                format!(
                                    "{h:04X} {h2:04X}  {}0x{target:08X}",
                                    if to_arm { "blx-arm " } else { "bl " }
                                ),
                            );
                            addr = addr.wrapping_add(4);
                            continue;
                        }
                    }
                    if h & 0xF000 == 0xD000 && (h >> 8) & 0xF < 0xE {
                        let target = addr
                            .wrapping_add(4)
                            .wrapping_add((((h as i8) as i32) * 2) as u32);
                        let cond = COND[((h >> 8) & 0xF) as usize];
                        lines.insert(addr, format!("{h:04X}  if {cond} goto 0x{target:08X}"));
                        if target >= base && target < text_end {
                            blocks.push(target);
                        }
                        addr = addr.wrapping_add(2);
                        continue;
                    }
                    if h & 0xF800 == 0xE000 {
                        let delta = ((((h & 0x07FF) << 5) as i16) >> 4) as i32;
                        let target = addr.wrapping_add(4).wrapping_add(delta as u32);
                        lines.insert(addr, format!("{h:04X}  goto 0x{target:08X}"));
                        if target >= base && target < text_end {
                            blocks.push(target);
                        }
                        break;
                    }
                    let text = decode_thumb(h, addr);
                    let text = if h & 0xFF87 == 0x4700 {
                        let rm = ((h >> 3) & 15) as usize;
                        if rm != 14 && thumb_restores_stack_before_bx(data, offset) {
                            format!("tailcall_indirect({});", REGS[rm])
                        } else {
                            text
                        }
                    } else {
                        text
                    };
                    lines.insert(addr, format!("{h:04X}  {text}"));
                    if h & 0xFF87 == 0x4700 && ((h >> 3) & 15) == 14
                        || h & 0xFE00 == 0xBC00 && h & 0x100 != 0
                    {
                        break;
                    }
                    if h & 0xFF87 == 0x4700 {
                        break;
                    }
                    if h & 0xFC00 == 0x4400 {
                        let op = (h >> 8) & 3;
                        let rd = (h & 7) | ((h >> 4) & 8);
                        if rd == 15 && op != 1 {
                            break;
                        }
                    }
                    addr = addr.wrapping_add(2);
                }
            }
        }
    }
    (lines, calls)
}

fn decompile_reachable(
    path: &Path,
    output: &Path,
    base: u32,
    text_size: usize,
    max_insns: usize,
) -> Result<(), String> {
    let data = fs::read(path).map_err(|e| e.to_string())?;
    let scan_len = text_size.min(data.len()) & !1;
    if scan_len < 4 {
        return Err("code image is too small".into());
    }
    let text_end = base
        .checked_add(scan_len as u32)
        .ok_or("text address range overflow")?;
    let entry = FuncKey {
        addr: base,
        mode: ArmMode::Arm,
    };
    let mut pending = vec![entry];
    let mut functions = BTreeMap::<FuncKey, BTreeMap<u32, String>>::new();
    while let Some(key) = pending.pop() {
        if functions.contains_key(&key) || functions.len() >= 20_000 {
            continue;
        }
        let (lines, calls) = trace_function(&data, base, text_end, key, max_insns);
        if lines.is_empty() {
            continue;
        }
        functions.insert(key, lines);
        pending.extend(
            calls
                .into_iter()
                .filter(|callee| !functions.contains_key(callee)),
        );
    }
    let mut listing = String::from(
        "// Reachability-guided ARM/Thumb pseudo-decompilation from the executable entry point. CFG edges and direct calls are followed; indirect calls, jump tables, and data/code boundaries remain heuristic.\n",
    );
    listing.push_str(&format!(
        "// Base: 0x{base:08X}; executable scan limit: 0x{scan_len:X}; reachable functions: {}\n\n",
        functions.len()
    ));
    let mut indirect_report = String::from(
        "These sites need runtime values or deeper points-to analysis. Nearby instructions are shown for triage; addresses are virtual addresses in the loaded executable.\n\n",
    );
    let mut indirect_counts = BTreeMap::<&str, usize>::new();
    for (key, lines) in &functions {
        let mode = if key.mode == ArmMode::Arm {
            "arm"
        } else {
            "thumb"
        };
        listing.push_str(&format!("void sub_{:08X}_{}(void) {{\n", key.addr, mode));
        for (&addr, text) in lines {
            listing.push_str(&format!("    /* {addr:08X} */ {text}\n"));
            let kind = if text.contains("tailcall_indirect(") {
                Some("Indirect tail calls")
            } else if text.contains("call_indirect(") {
                Some("Indirect calls")
            } else if text.contains("branch_indirect(") {
                Some("Indirect branches")
            } else {
                None
            };
            if let Some(kind) = kind {
                *indirect_counts.entry(kind).or_default() += 1;
                indirect_report.push_str(&format!(
                    "## {kind}: 0x{addr:08X} in `sub_{:08X}_{mode}`\n\n",
                    key.addr
                ));
                indirect_report.push_str("```text\n");
                let width = if key.mode == ArmMode::Arm { 4 } else { 2 };
                for delta in [-2i64, -1, 0, 1, 2] {
                    let context_addr = if delta < 0 {
                        addr.checked_sub(((-delta) as u32) * width)
                    } else {
                        addr.checked_add((delta as u32) * width)
                    };
                    if let Some(context_addr) = context_addr.and_then(|a| lines.get_key_value(&a)) {
                        indirect_report
                            .push_str(&format!("0x{:08X}: {}\n", context_addr.0, context_addr.1));
                    }
                }
                indirect_report.push_str("```\n\n");
            }
        }
        listing.push_str("}\n\n");
    }
    if let Some(parent) = output.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    fs::write(output, listing).map_err(|e| e.to_string())?;
    indirect_report.insert_str(
        0,
        &format!(
            "# Unresolved indirect control-flow sites\n\nCurrent reachable graph contains {} functions.\n\n",
            functions.len()
        ),
    );
    indirect_report.push_str("## Counts\n\n");
    for (kind, count) in indirect_counts {
        indirect_report.push_str(&format!("- {kind}: {count}\n"));
    }
    let report_path = output.with_extension("indirect.md");
    fs::write(&report_path, indirect_report).map_err(|e| e.to_string())?;
    println!(
        "Wrote {} reachable ARM/Thumb functions to {} and indirect-site report to {}",
        functions.len(),
        output.display(),
        report_path.display()
    );
    Ok(())
}

fn decode_thumb(h: u16, addr: u32) -> String {
    if h & 0xE000 == 0 {
        let kind = (h >> 11) & 3;
        if kind < 3 {
            let names = ["lsl", "lsr", "asr"];
            let imm = (h >> 6) & 31;
            return format!(
                "r{} = {}(r{}, {imm});",
                h & 7,
                names[kind as usize],
                (h >> 3) & 7
            );
        }
    }
    if h & 0xF800 == 0x1800 {
        let imm = h & (1 << 10) != 0;
        let sub = h & (1 << 9) != 0;
        let dst = h & 7;
        let lhs = (h >> 3) & 7;
        let rhs = if imm {
            format!("0x{:X}", (h >> 6) & 7)
        } else {
            format!("r{}", (h >> 6) & 7)
        };
        return format!("r{dst} = r{lhs} {} {rhs};", if sub { "-" } else { "+" });
    }
    if h & 0xF800 == 0x2000 {
        return format!("r{} = 0x{:02X};", (h >> 8) & 7, h & 0xFF);
    }
    if h & 0xF800 == 0x2800 {
        return format!("compare(r{}, 0x{:02X});", (h >> 8) & 7, h & 0xFF);
    }
    if h & 0xF800 == 0x3000 || h & 0xF800 == 0x3800 {
        let op = if h & 0x0800 == 0 { "+=" } else { "-=" };
        return format!("r{} {op} 0x{:02X};", (h >> 8) & 7, h & 0xFF);
    }
    if h & 0xF800 == 0x4800 {
        let literal = (addr.wrapping_add(4) & !3).wrapping_add(((h & 0xFF) as u32) << 2);
        return format!("r{} = *(0x{literal:08X});", (h >> 8) & 7);
    }
    if h & 0xFC00 == 0x4000 {
        let ops = [
            "and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror", "tst", "neg", "cmp", "cmn",
            "orr", "mul", "bic", "mvn",
        ];
        let op = ops[((h >> 6) & 15) as usize];
        let rd = h & 7;
        let rm = (h >> 3) & 7;
        return match op {
            "tst" | "cmp" | "cmn" => format!("{op}(r{rd}, r{rm});"),
            "neg" => format!("r{rd} = -r{rm};"),
            "mvn" => format!("r{rd} = ~r{rm};"),
            _ => format!("r{rd} = {op}(r{rd}, r{rm});"),
        };
    }
    if h & 0xFC00 == 0x4400 {
        let op = (h >> 8) & 3;
        let rd = (h & 7) | ((h >> 4) & 8);
        let rm = (h >> 3) & 15;
        return match op {
            0 if rm == 15 => {
                let pc = addr.wrapping_add(4);
                format!("r{rd} += 0x{pc:08X};")
            }
            0 => format!("r{rd} += r{rm};"),
            1 => format!("compare(r{rd}, r{rm});"),
            2 => format!("r{rd} = r{rm};"),
            3 if rm == 14 => "return;".to_string(),
            3 if h & 0x0080 != 0 => format!("call_indirect(r{rm});"),
            _ => format!("branch(r{rm});"),
        };
    }
    if h & 0xF000 == 0x5000 {
        let ops = [
            "str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh",
        ];
        let op = ops[((h >> 9) & 7) as usize];
        let rd = h & 7;
        let rb = (h >> 3) & 7;
        let ro = (h >> 6) & 7;
        return if op.starts_with("str") {
            format!("*(r{rb} + r{ro}) = r{rd}; // {op}")
        } else {
            format!("r{rd} = *({op} r{rb} + r{ro});")
        };
    }
    if h & 0xE000 == 0x6000 {
        let byte = h & (1 << 12) != 0;
        let load = h & (1 << 11) != 0;
        let size = if byte { 1 } else { 4 };
        let off = ((h >> 6) & 31) * size;
        let rb = (h >> 3) & 7;
        let rd = h & 7;
        return if load {
            format!("r{rd} = *(r{rb} + 0x{off:X});")
        } else {
            format!("*(r{rb} + 0x{off:X}) = r{rd};")
        };
    }
    if h & 0xF000 == 0x8000 {
        let load = h & (1 << 11) != 0;
        let off = ((h >> 6) & 31) * 2;
        let rb = (h >> 3) & 7;
        let rd = h & 7;
        return if load {
            format!("r{rd} = *(u16 *)(r{rb} + 0x{off:X});")
        } else {
            format!("*(u16 *)(r{rb} + 0x{off:X}) = r{rd};")
        };
    }
    if h & 0xF000 == 0x9000 {
        let load = h & (1 << 11) != 0;
        let rd = (h >> 8) & 7;
        let off = (h & 0xFF) * 4;
        return if load {
            format!("r{rd} = *(sp + 0x{off:X});")
        } else {
            format!("*(sp + 0x{off:X}) = r{rd};")
        };
    }
    if h & 0xFF00 == 0xA000 {
        let base = if h & (1 << 11) != 0 { "sp" } else { "pc" };
        return format!("r{} = {base} + 0x{:X};", (h >> 8) & 7, (h & 0xFF) * 4);
    }
    if h & 0xFF00 == 0xB000 {
        let off = ((h & 0x7F) as u32) << 2;
        return format!("sp {}= 0x{off:X};", if h & 0x80 != 0 { "-" } else { "+" });
    }
    if h & 0xF000 == 0xC000 {
        let load = h & (1 << 11) != 0;
        let rb = (h >> 8) & 7;
        let list = h & 0xFF;
        return format!(
            "{} r{rb}!, register_list=0x{list:02X};",
            if load { "ldmia" } else { "stmia" }
        );
    }
    if h & 0xF000 == 0xD000 && (h >> 8) & 0xF < 0xE {
        let cond = COND[((h >> 8) & 0xF) as usize];
        let delta = (((h as i8) as i32) * 2) as u32;
        return format!(
            "if ({cond}) goto 0x{:08X};",
            addr.wrapping_add(4).wrapping_add(delta)
        );
    }
    if h & 0xF800 == 0xE000 {
        let delta = ((((h & 0x07FF) << 5) as i16) >> 4) as i32 as u32;
        return format!("goto 0x{:08X};", addr.wrapping_add(4).wrapping_add(delta));
    }
    if h & 0xFF87 == 0x4700 {
        return format!("return_or_branch({});", REGS[((h >> 3) & 15) as usize]);
    }
    if h & 0xFE00 == 0xB400 {
        return format!(
            "push(register_list=0x{:02X}{});",
            h & 0xFF,
            if h & 0x100 != 0 { ", lr" } else { "" }
        );
    }
    if h & 0xFE00 == 0xBC00 {
        return format!(
            "pop(register_list=0x{:02X}{});",
            h & 0xFF,
            if h & 0x100 != 0 { ", pc" } else { "" }
        );
    }
    format!("/* unsupported Thumb 0x{h:04X} */")
}

fn disasm(path: &Path, base: u32, mode: &str, count: usize, blz: bool) -> Result<(), String> {
    let mut data = fs::read(path).map_err(|e| e.to_string())?;
    if blz {
        data = blz_decompress(&data)?;
    }
    let width = if mode == "arm" { 4 } else { 2 };
    let n = count.min(data.len() / width);
    for i in 0..n {
        let addr = base.wrapping_add((i * width) as u32);
        let b = &data[i * width..(i + 1) * width];
        let ins = if width == 4 {
            decode_arm(u32::from_le_bytes(b.try_into().unwrap()), addr)
        } else {
            decode_thumb(u16::from_le_bytes(b.try_into().unwrap()), addr)
        };
        println!(
            "0x{addr:08X}: {:<11} {ins}",
            b.iter()
                .map(|x| format!("{x:02x}"))
                .collect::<Vec<_>>()
                .join(" ")
        );
    }
    if n == 0 {
        println!("No complete instructions to decode.");
    }
    Ok(())
}

fn usage() -> ! {
    eprintln!(
        "dq8tool inspect IMAGE\n  dq8tool decrypt-code IMAGE OUTPUT [--partition N] [--ctrtool PATH] [--keyset PATH]\n  dq8tool decompile CODE OUTPUT [--base 0xADDR] [--text-size SIZE] [--max-insns N]\n  dq8tool recompile-entry CODE OUTPUT.c [--base 0xADDR]\n  dq8tool recompile-arm CODE ADDRESS OUTPUT.c [--base 0xADDR] [--s1-block 0xADDR] [--s1-cfg 0xADDR --s1-cfg-end 0xADDR]\n  dq8tool extract-code IMAGE OUTPUT [--partition N] [--decompress-blz]\n  dq8tool extract-blocks CODE START END [--base 0xADDR] [--out FILE] [--check-roundtrip]\n  dq8tool extract-sarc ARCHIVE OUTPUT_DIR\n  dq8tool inspect-layout LAYOUT.bflyt\n  dq8tool disasm CODE [--mode arm|thumb] [--base 0xADDR] [--count N] [--decompress-blz]"
    );
    process::exit(2)
}

// First executable static-recompilation slice: translate DQ8's ARM reset/BSS
// initializer (the first directly-called leaf at 0x00100024) into host C.
// We validate the actual opcode stream and decode its PC-relative literals;
// the output is compiled by the host C compiler and executes the same zeroing
// loop over a guest-addressed RAM image.
fn recompile_entry(code: &Path, output: &Path, base: u32) -> Result<(), String> {
    let bytes = fs::read(code).map_err(|e| format!("cannot read {}: {e}", code.display()))?;
    let start = 0x0010_0024u32;
    if base != 0x0010_0000 || start < base {
        return Err(
            "this initial translator supports DQ8's ARM reset helper at base 0x00100000".into(),
        );
    }
    let off = (start - base) as usize;
    if off + 0x20 > bytes.len() {
        return Err("code image does not contain the reset helper".into());
    }
    let words: Vec<u32> = bytes[off..off + 0x1c]
        .chunks_exact(4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .collect();
    let expected = [
        0xE59F0014, 0xE59F1014, 0xE3A02000, 0xE1500001, 0x34802004, 0x3AFFFFFC, 0xE12FFF1E,
    ];
    if words.as_slice() != expected {
        return Err(format!(
            "unexpected opcodes at 0x{start:08X}; refusing to emit guessed native code"
        ));
    }
    let lit0 = u32::from_le_bytes(bytes[0x40..0x44].try_into().unwrap());
    let lit1 = u32::from_le_bytes(bytes[0x44..0x48].try_into().unwrap());
    if lit0 >= lit1 || lit1 > 0x0100_0000 {
        return Err(format!(
            "BSS range 0x{lit0:08X}..0x{lit1:08X} is outside the supported guest RAM window"
        ));
    }
    let source = format!(
        r#"/* Generated from DQ8 ARM instructions at 0x00100024.
 * This is native host code for the guest BSS initialization routine.
 */
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t guest_ram[0x01000000];
static void dq8_arm_00100024(void) {{
    uint32_t r0 = 0x{lit0:08X}; /* LDR r0, [pc, #0x14], literal at 0x00100040 */
    uint32_t r1 = 0x{lit1:08X}; /* LDR r1, [pc, #0x14], literal at 0x00100044 */
    uint32_t r2 = 0;            /* MOV r2, #0 */
    while (r0 < r1) {{          /* CMP; STRCC; ADD; BCC */
        *(uint32_t *)(void *)(guest_ram + r0) = r2;
        r0 += 4;
    }}
}}

int main(void) {{
    dq8_arm_00100024();
    for (uint32_t a = 0x{lit0:08X}; a < 0x{lit1:08X}; a += 4)
        if (*(uint32_t *)(void *)(guest_ram + a) != 0) return 1;
    printf("executed recompiled DQ8 ARM routine 0x00100024; zeroed 0x%X guest bytes\n", 0x{size:X});
    return 0;
}}
"#,
        size = lit1 - lit0
    );
    if let Some(parent) = output.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent).map_err(|e| format!("cannot create output directory: {e}"))?;
    }
    fs::write(output, source).map_err(|e| format!("cannot write {}: {e}", output.display()))?;
    println!(
        "Generated native C for validated DQ8 ARM routine 0x{start:08X} -> {}",
        output.display()
    );
    Ok(())
}

// Wrap one lowered instruction body in its ARM condition.
//
// The dispatcher case is `case 0x<pc>: { <stmt> break; }`.  For a conditionally
// executed instruction the statement has to be
//
//     if (<cond>) { <body> } else { pc = <fallthrough>; }
//
// because a failed condition must continue at the next address.  For an
// UNconditional instruction (`cond` 14/15, i.e. the literal condition "1") the
// else arm can never be reached, yet it used to be emitted too: 88.6% of the
// whole image carried a second `pc` constant and a duplicate fall-through store
// that the front end and every optimiser pass still had to parse, number and
// reason about.  clang folds `if (1)` away, so the machine code is unchanged --
// the cost was the translation unit's size and the build time it buys.  When the
// condition is statically true the body alone is the same program -- same
// architectural state, same touched memory, same PC on exit -- so it is emitted
// unwrapped.
fn condition_wrap(condition: &str, body: &str, fallthrough: &str) -> String {
    if condition == "1" {
        body.to_string()
    } else {
        format!("if ({condition}) {{ {body} }} else {{ {fallthrough} }}")
    }
}

fn arm_condition_c(cond: u32) -> &'static str {
    match cond {
        0 => "z",
        1 => "!z",
        2 => "c",
        3 => "!c",
        4 => "n",
        5 => "!n",
        6 => "v",
        7 => "!v",
        8 => "c && !z",
        9 => "!c || z",
        10 => "n == v",
        11 => "n != v",
        12 => "!z && n == v",
        13 => "z || n != v",
        14 => "1",
        _ => "0",
    }
}

// VFP double-precision (F64) helpers for the emitted C (audit R2-01).
// A d-register occupies two adjacent 32-bit fpr words: the low word at
// `slot`, the high word at `slot + 1`.  All reads happen inside a single
// expression so Rd-aliases (Vd == Vn/Vm) observe the operands before the
// destination pair is written back.
fn vfp_f64_read(slot: usize) -> String {
    format!(
        "f64_from_bits(((uint64_t)fpr[{h}] << 32) | fpr[{l}])",
        h = slot + 1,
        l = slot
    )
}

fn vfp_f64_store(slot: usize, expr: &str) -> String {
    format!(
        "{{ uint64_t _f64r = f64_to_bits({expr}); \
           fpr[{l}] = (uint32_t)_f64r; fpr[{h}] = (uint32_t)(_f64r >> 32); }}",
        expr = expr,
        l = slot,
        h = slot + 1
    )
}

// Decode Dd/Dn/Dm for VFP double data-processing into fpr slot pairs.
// D:Vd >= 16 selects d16+, which is undefined on the 16-double (32-word
// fpr) VFP bank this runtime models - index 32 would alias fpscr.  Such
// encodings must be rejected loudly (guest_unsupported), never silently
// executed against neighbouring state (audit R2-01 forbids silent
// lowering; an explicit error is the accepted alternative).
fn vfp_double_slots(w: u32) -> Option<(usize, usize, usize)> {
    let d = (((w >> 12) & 15) | (((w >> 22) & 1) << 4)) as usize;
    let n = (((w >> 16) & 15) | (((w >> 7) & 1) << 4)) as usize;
    let m = ((w & 15) | (((w >> 5) & 1) << 4)) as usize;
    if d >= 16 || n >= 16 || m >= 16 {
        None
    } else {
        Some((d * 2, n * 2, m * 2))
    }
}

// Recognize the constant-return paths used by DQ8's startup metadata helpers.
// The second form is valid only when the caller proves r1 == 0: its zero/null
// branch skips the string lookup and returns a PC-relative constant.
fn arm_constant_return(data: &[u8], base: u32, start: u32, allow_zero_arg: bool) -> Option<u32> {
    let word = |addr: u32| -> Option<u32> {
        let off = addr.checked_sub(base)? as usize;
        Some(u32::from_le_bytes(data.get(off..off + 4)?.try_into().ok()?))
    };
    let w0 = word(start)?;
    if w0 & 0x0C5F_F000 == 0x041F_0000 && (w0 >> 28) == 14 && (w0 >> 12) & 15 == 0 {
        if word(start + 4)? & 0x0FFF_FFF0 != 0x012F_FF10 || word(start + 4)? & 15 != 14 {
            return None;
        }
        let imm = w0 & 0xFFF;
        let literal = if w0 & (1 << 23) != 0 {
            start + 8 + imm
        } else {
            start + 8 - imm
        };
        return word(literal);
    }
    if !allow_zero_arg || w0 != 0xE92D_4010 || word(start + 4)? != 0xE351_0000 {
        return None;
    }
    let (branch_target, link, _) = arm_branch_target(word(start + 8)?, start + 8)?;
    if link {
        return None;
    }
    let ldr_addr = branch_target;
    let ldr = word(ldr_addr)?;
    let add = word(ldr_addr + 4)?;
    let pop = word(ldr_addr + 8)?;
    if ldr & 0x0C5F_F000 != 0x041F_0000
        || ldr >> 28 != 14
        || (ldr >> 12) & 15 != 0
        || add != 0xE08F_0000
        || pop != 0xE8BD_8010
    {
        return None;
    }
    let imm = ldr & 0xFFF;
    let literal = if ldr & (1 << 23) != 0 {
        ldr_addr + 8 + imm
    } else {
        ldr_addr + 8 - imm
    };
    Some((ldr_addr + 4 + 8).wrapping_add(word(literal)?))
}

// Emit native C basic-block dispatch for an ARM function. Every emitted case
// is a direct lowering of one source instruction; unsupported encodings stop
// translation instead of silently becoming no-ops.
fn looks_like_thumb_entry(bytes: &[u8], base: u32, addr: u32) -> bool {
    let Some(off) = addr.checked_sub(base).map(|x| x as usize) else {
        return false;
    };
    if off + 4 > bytes.len() {
        return false;
    }
    let first = u16::from_le_bytes(bytes[off..off + 2].try_into().unwrap());
    let second = u16::from_le_bytes(bytes[off + 2..off + 4].try_into().unwrap());
    first & 0xF800 == 0xF000
        && (second & 0xD000 == 0xD000 || (second & 0xD000 == 0xC000 && second & 1 == 0))
}

fn thumb_bl_target(addr: u32, first: u16, second: u16) -> Option<(u32, bool)> {
    let suffix = second & 0xD000;
    if first & 0xF800 != 0xF000
        || (suffix != 0xD000 && suffix != 0xC000)
        || (suffix == 0xC000 && second & 1 != 0)
    {
        return None;
    }
    let sign = ((first >> 10) & 1) as u32;
    let j1 = ((second >> 13) & 1) as u32;
    let j2 = ((second >> 11) & 1) as u32;
    let i1 = 1 ^ (j1 ^ sign);
    let i2 = 1 ^ (j2 ^ sign);
    let to_arm = suffix == 0xC000;
    let low = if to_arm {
        (((second >> 1) & 0x03FF) as u32) << 2
    } else {
        ((second & 0x07FF) as u32) << 1
    };
    let bits = (sign << 24) | (i1 << 23) | (i2 << 22) | (((first & 0x03FF) as u32) << 12) | low;
    let offset = ((bits << 7) as i32 >> 7) as u32;
    let pc = addr.wrapping_add(4);
    let target = if to_arm {
        (pc & !3).wrapping_add(offset) & !3
    } else {
        pc.wrapping_add(offset) & !1
    };
    Some((target, to_arm))
}

fn looks_like_a32_instruction_middle(bytes: &[u8], base: u32, addr: u32) -> bool {
    if addr & 3 != 2 || addr < base + 2 {
        return false;
    }
    let off = (addr - 2 - base) as usize;
    if off + 4 > bytes.len() {
        return false;
    }
    let word = u32::from_le_bytes(bytes[off..off + 4].try_into().unwrap());
    let coprocessor =
        word >> 28 != 15 && word & 0x0C00_0000 == 0x0C00_0000 && (word >> 8) & 15 == 10;
    let branch = word >> 28 != 15 && word & 0x0E00_0000 == 0x0A00_0000;
    let target_off = (addr - base) as usize;
    let first = u16::from_le_bytes(bytes[target_off..target_off + 2].try_into().unwrap());
    let second = u16::from_le_bytes(bytes[target_off + 2..target_off + 4].try_into().unwrap());
    let invalid_thumb_add = first & 0xFF00 == 0xEB00
        && !matches!(first & 0xFF80, 0xEB00 | 0xEBA0)
        && (second & 0x8000 == 0 || second & 0xF800 == 0x5000);
    coprocessor || (branch && invalid_thumb_add)
}

fn looks_like_arm_entry(bytes: &[u8], base: u32, addr: u32) -> bool {
    if addr & 3 != 0 || addr < base {
        return false;
    }
    let off = (addr - base) as usize;
    if off + 12 > bytes.len() {
        return false;
    }
    let words: Vec<u32> = (0..3)
        .map(|i| u32::from_le_bytes(bytes[off + i * 4..off + i * 4 + 4].try_into().unwrap()))
        .collect();
    if matches!(words[0], 0xF57F_F01F | 0xE1A0_0000 | 0xE320_F000)
        || (words[0] & 0x0C5F_F000 == 0x041F_F000 && (words[0] >> 12) & 15 == 15)
    {
        return true;
    }
    let prologue_markers = words
        .iter()
        .filter(|&&w| {
            w == 0xF57F_F01F // CLREX
                || w == 0xE1A0_0000 // MOV r0,r0 / NOP
                || w == 0xE320_F000 // ARMv6 NOP hint
                || w & 0x0FFF_0000 == 0x092D_0000 // PUSH {..,lr}
                || w & 0x0FFF_0000 == 0x08BD_0000 // POP {..,pc}
                || (w >> 28 == 0xE && w & 0x0E00_0000 == 0x0A00_0000) // unconditional B/BL
        })
        .count();
    prologue_markers >= 2
}

// Recover the common ARM switch idiom `CMP index, #count; LDRCC pc,
// [pc, index, LSL #2]`. The table is embedded immediately after the load and
// contains absolute guest addresses. Enqueue each table destination as a
// translation seed so computed branches do not escape the generated image.
fn arm_pc_relative_jump_table_targets(
    bytes: &[u8],
    base: u32,
    addr: u32,
    instruction: u32,
) -> Vec<(u32, bool)> {
    if instruction & 0x0FFF_F000 != 0x079F_F000
        || (instruction >> 28) != 3 // BCC guards the table index
        || (instruction >> 7) & 0x1F != 2 // LSL #2
        || (instruction >> 5) & 3 != 0
        || instruction & (1 << 4) != 0
    {
        return Vec::new();
    }
    let index_reg = (instruction & 15) as usize;
    let mut count = None;
    for distance in 1..=8u32 {
        let Some(candidate_addr) = addr.checked_sub(distance * 4) else {
            break;
        };
        let Some(off) = candidate_addr.checked_sub(base).map(|v| v as usize) else {
            continue;
        };
        let Some(raw) = bytes.get(off..off + 4) else {
            continue;
        };
        let candidate = u32::from_le_bytes(raw.try_into().unwrap());
        let is_cmp_imm = (candidate >> 25) & 7 == 1
            && (candidate >> 21) & 15 == 10
            && candidate & (1 << 20) != 0
            && ((candidate >> 16) & 15) as usize == index_reg;
        if is_cmp_imm {
            let rotate = ((candidate >> 8) & 15) * 2;
            let bound = (candidate & 0xFF).rotate_right(rotate);
            if bound == 0 || bound > 256 {
                return Vec::new();
            }
            count = Some(bound);
            break;
        }
    }
    let Some(count) = count else {
        return Vec::new();
    };
    let Some(table_start) = addr.checked_add(8).and_then(|v| v.checked_sub(base)) else {
        return Vec::new();
    };
    let table_start = table_start as usize;
    let mut targets = Vec::with_capacity(count as usize);
    for index in 0..count as usize {
        let Some(off) = table_start.checked_add(index * 4) else {
            break;
        };
        let Some(raw) = bytes.get(off..off + 4) else {
            break;
        };
        let target = u32::from_le_bytes(raw.try_into().unwrap());
        let target_addr = target & !1;
        if target_addr >= base && (target_addr - base) as usize + 4 <= bytes.len() {
            targets.push((target_addr, target & 1 != 0));
        }
    }
    targets
}

// Enqueue a translation seed whose instruction-set state the decoder has
// already established (a BL/BX/table entry inside A32 or Thumb code), as
// opposed to a bare address from recomp-roots.txt where the mode has to be
// inferred from the bytes.
//
// An explicitly ARM seed must NOT be re-classified by looks_like_thumb_entry():
// that heuristic byte-swaps the first word, so an A32 `BL` such as EB04F213
// reads as the Thumb pair 0xF213/0xEB04 - a valid Thumb BLX.  Classifying the
// seed as Thumb emits the block only under the odd (Thumb) key, so the guest's
// even ARM address has no case and the dispatch escapes.  That is exactly the
// 0x002B007C battle escape: the conditional switch `LDRCC pc,[pc,r0,lsl#2]` at
// 0x002B001C seeds the case-2 body 0x002B007C, whose first instruction is
// `BL 0x003EC8D0`, and the body was translated in Thumb state instead.
// `force_a32` is the existing mechanism that pins a seed to A32 decoding.
fn enqueue_known_seed(
    pending: &mut Vec<(u32, bool)>,
    force_a32: &mut BTreeSet<u32>,
    target: u32,
    thumb: bool,
) {
    if !thumb {
        force_a32.insert(target & !1);
    }
    pending.push((target & !1, thumb));
}

#[cfg(test)]
fn recompile_arm_function(
    code: &Path,
    output: &Path,
    base: u32,
    start: u32,
    extra_entries: &[u32],
) -> Result<(), String> {
    recompile_arm_function_with_noreturn(code, output, base, start, extra_entries, &[], None, None)
}

/// S1 rollout: every region converted in ONE build (the multi-region
/// `DQ8_S1_CFG` gate), with the concatenated C splices for the three
/// `S1_CFG_*` markers.  `entries` are the region entries (the A05 boundary
/// set and the runtime gate both need them).
struct S1CfgRegions {
    entries: Vec<u32>,
    function_c: String,
    hook_c: String,
    gate_c: String,
}

fn recompile_arm_function_with_noreturn(
    code: &Path,
    output: &Path,
    base: u32,
    start: u32,
    extra_entries: &[u32],
    noreturn_calls: &[u32],
    s1_block: Option<u32>,
    s1_cfg: Option<Vec<(u32, u32)>>,
) -> Result<(), String> {
    let bytes = fs::read(code).map_err(|e| format!("cannot read {}: {e}", code.display()))?;
    if start < base || start as usize + 4 > base as usize + bytes.len() {
        return Err("function start is outside the code image".into());
    }
    let mut emitted = BTreeMap::<u32, String>::new();
    let mut pending = vec![(start & !1, start & 1 != 0)];
    let noreturn_calls: BTreeSet<u32> = noreturn_calls.iter().map(|addr| addr & !1).collect();
    // Thumb-2 VFP delegation (t_4734a0a2).  A Thumb-2 coprocessor instruction
    // is the A32 encoding with its two halfwords swapped (llvm-mc: thumb
    // `vcvt.f32.f64 s0, d0` = hw1 0xEEB7 / hw2 0x0BC0, A32 0xEEB70BC0), so it
    // is decoded exactly once, by the A32 VFP chain, from the byte-swapped
    // word recorded here - there is no second VFP decoder to keep in sync.
    // `force_a32` makes the re-entry decode in A32 state instead of running the
    // Thumb-entry heuristics over the swapped bytes.
    let mut force_a32 = BTreeSet::<u32>::new();
    let mut a32_word = BTreeMap::<u32, u32>::new();
    pending.extend(
        extra_entries
            .iter()
            .copied()
            .map(|entry| (entry & !1, entry & 1 != 0)),
    );
    let mut functions = BTreeSet::new();
    let mut total_insns = 0usize;
    while let Some((function_start, requested_thumb)) = pending.pop() {
        if requested_thumb && looks_like_a32_instruction_middle(&bytes, base, function_start) {
            continue;
        }
        // Some ARM functions are reached through interworking branches whose
        // low bit was lost by an earlier decoded call.  Strong A32 prologues
        // (CLREX/NOP/PUSH) must override the Thumb halfword heuristic too.
        let arm_entry = looks_like_arm_entry(&bytes, base, function_start);
        let thumb_mode = !force_a32.contains(&function_start)
            && !arm_entry
            && (requested_thumb || looks_like_thumb_entry(&bytes, base, function_start));
        if !functions.insert((function_start, thumb_mode)) {
            continue;
        }
        let mut addr = function_start;
        let mut ended = false;
        let mut known_zero = [false; 16];
        for _ in 0..4096 {
            let pc_key = addr | u32::from(thumb_mode);
            if !thumb_mode && addr == 0x00131388 {
                // The guest's recursive-mutex slow path blocks until a peer
                // thread wakes it. This runner has no concurrent scheduler;
                // grant the lock so startup can proceed in its sole thread.
                emitted.insert(pc_key, "pc = r[14];".to_string());
                ended = true;
                break;
            }
            if emitted.contains_key(&pc_key) {
                ended = true;
                break;
            }
            if thumb_mode {
                let off = (addr - base) as usize;
                if off + 2 > bytes.len() {
                    break;
                }
                let h = u16::from_le_bytes(bytes[off..off + 2].try_into().unwrap());
                let mut next = addr.wrapping_add(2);
                let mut next_key = next | 1;
                let mut body = String::new();
                let mut condition = "1".to_string();
                let mut changes_pc = false;
                let aligned_a32 = if addr & 3 == 0 && off + 4 <= bytes.len() {
                    u32::from_le_bytes(bytes[off..off + 4].try_into().unwrap())
                } else {
                    0
                };
                if matches!(h & 0xF800, 0xF800)
                    && (aligned_a32 & 0x0FFF_FFF0 == 0x012F_FF10
                        || aligned_a32 & 0x0FFF_FFF0 == 0x012F_FF30)
                {
                    // Interworking can land on A32 code after an indirect
                    // BX/BLX. Re-decode this aligned word in ARM state.
                    pending.push((addr, false));
                    body.push_str(&format!("pc = 0x{addr:08X}u;"));
                    changes_pc = true;
                    ended = true;
                } else if h & 0xFE00 == 0xB400 {
                    let list = h & 0xFF;
                    let with_lr = h & 0x100 != 0;
                    let count = list.count_ones() + u32::from(with_lr);
                    body.push_str(&format!("r[13] -= {}u;", count * 4));
                    let mut i = 0;
                    for reg in 0..8 {
                        if list & (1 << reg) != 0 {
                            body.push_str(&format!("put32(r[13] + {}u, r[{reg}]);", i * 4));
                            i += 1;
                        }
                    }
                    if with_lr {
                        body.push_str(&format!("put32(r[13] + {}u, r[14]);", i * 4));
                    }
                } else if h & 0xFE00 == 0xBC00 {
                    let list = h & 0xFF;
                    let with_pc = h & 0x100 != 0;
                    let mut i = 0;
                    for reg in 0..8 {
                        if list & (1 << reg) != 0 {
                            body.push_str(&format!("r[{reg}] = mem32(r[13] + {}u);", i * 4));
                            i += 1;
                        }
                    }
                    if with_pc {
                        body.push_str(&format!(
                            "pc = mem32(r[13] + {}u); if (pc == 0xFFFFFFFFu) running = 0;",
                            i * 4
                        ));
                        i += 1;
                        changes_pc = true;
                        ended = true;
                    }
                    body.push_str(&format!("r[13] += {}u;", i * 4));
                } else if h & 0xFF00 == 0xB000 {
                    let delta = ((h & 0x7F) * 4) as u32;
                    if h & 0x80 != 0 {
                        body.push_str(&format!("r[13] -= {delta}u;"));
                    } else {
                        body.push_str(&format!("r[13] += {delta}u;"));
                    }
                } else if matches!(h & 0xF800, 0x0000 | 0x0800 | 0x1000) {
                    // Thumb immediate shifts (LSLS/LSRS/ASRS rd, rm, #imm).
                    // Snapshot the source before writing Rd (Rd may alias Rm)
                    // and route result and carry through the shared
                    // immediate-shift helpers so the encoded amount 0 keeps
                    // its architectural meaning (LSL #0 = no shift, LSR/ASR
                    // #32) and the generated C never shifts a 32-bit value by
                    // its width (audit A02).
                    let kind = match h & 0xF800 {
                        0x0000 => 0,
                        0x0800 => 1,
                        _ => 2,
                    };
                    let amount = ((h >> 6) & 31) as u32;
                    let rm = ((h >> 3) & 7) as usize;
                    let rd = (h & 7) as usize;
                    body.push_str(&format!(
                        "{{ uint32_t _sh=r[{rm}]; r[{rd}]=arm_shift_imm(_sh, {kind}u, {amount}u, c); c=arm_shift_imm_carry(_sh, {kind}u, {amount}u, c); z=(r[{rd}]==0); n=(r[{rd}]>>31); }}"
                    ));
                } else if h & 0xFC00 == 0x4000 {
                    let op = ((h >> 6) & 15) as usize;
                    let rm = ((h >> 3) & 7) as usize;
                    let rd = (h & 7) as usize;
                    match op {
                        0 | 1 | 12 | 14 => {
                            let expr = match op {
                                0 => format!("r[{rd}] & r[{rm}]"),
                                1 => format!("r[{rd}] ^ r[{rm}]"),
                                12 => format!("r[{rd}] | r[{rm}]"),
                                _ => format!("r[{rd}] & ~r[{rm}]"),
                            };
                            body.push_str(&format!(
                                "r[{rd}] = ({expr}); z=(r[{rd}]==0); n=(r[{rd}]>>31);"
                            ));
                        }
                        2 | 3 | 4 | 7 => {
                            let kind = match op {
                                2 => 0,
                                3 => 1,
                                4 => 2,
                                _ => 3,
                            };
                            body.push_str(&format!("{{ unsigned amount=r[{rm}] & 255u; c=arm_shift_carry(r[{rd}], {kind}u, amount, c); r[{rd}]=arm_shift(r[{rd}], {kind}u, amount); z=(r[{rd}]==0); n=(r[{rd}]>>31); }}"));
                        }
                        5 | 6 => {
                            // Thumb ADCS/SBCS: form the full wide sum (or
                            // borrow) before truncating, so the carry out of
                            // b + carry-in is not lost when that add wraps,
                            // and derive overflow from the signed wide value
                            // (audit A04; same semantics as the ARM
                            // data-processing emitters below).
                            if op == 6 {
                                body.push_str(&format!(
                                    "{{ uint32_t a=r[{rd}], b=r[{rm}], borrow=(uint32_t)!c; uint64_t sub=(uint64_t)b+borrow; uint32_t t=(uint32_t)((uint64_t)a-sub); int64_t sw=(int64_t)(int32_t)a-(int64_t)(int32_t)b-borrow; r[{rd}]=t; z=(t==0); n=(t>>31); c=((uint64_t)a>=sub); v=(sw>INT32_MAX || sw<INT32_MIN); }}"
                                ));
                            } else {
                                body.push_str(&format!(
                                    "{{ uint32_t a=r[{rd}], b=r[{rm}], oldc=(uint32_t)c; uint64_t wide=(uint64_t)a+b+oldc; uint32_t t=(uint32_t)wide; int64_t sw=(int64_t)(int32_t)a+(int64_t)(int32_t)b+oldc; r[{rd}]=t; z=(t==0); n=(t>>31); c=(wide>>32); v=(sw>INT32_MAX || sw<INT32_MIN); }}"
                                ));
                            }
                        }
                        8 => {
                            // Thumb TST is a logical AND test: only N/Z are
                            // written and C/V are preserved.  It shares the
                            // 0x4000 decode block with CMP but must not
                            // perform the subtraction (audit A03).
                            body.push_str(&format!(
                                "{{ uint32_t t=r[{rd}] & r[{rm}]; z=(t==0); n=(t>>31); }}"
                            ));
                        }
                        10 => {
                            body.push_str(&format!("{{ uint32_t a=r[{rd}], b=r[{rm}], t=a-b; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}"));
                        }
                        9 => {
                            body.push_str(&format!("{{ uint32_t b=r[{rm}], t=0u-b; r[{rd}]=t; z=(t==0); n=(t>>31); c=(b==0); v=(b==0x80000000u); }}"));
                        }
                        11 => {
                            body.push_str(&format!("{{ uint32_t a=r[{rd}], b=r[{rm}], t=a+b; uint64_t wide=(uint64_t)a+b; z=(t==0); n=(t>>31); c=(wide>>32); v=((~(a^b)&(a^t))>>31); }}"));
                        }
                        13 => {
                            body.push_str(&format!(
                                "r[{rd}] *= r[{rm}]; z=(r[{rd}]==0); n=(r[{rd}]>>31);"
                            ));
                        }
                        15 => {
                            body.push_str(&format!(
                                "r[{rd}]=~r[{rm}]; z=(r[{rd}]==0); n=(r[{rd}]>>31);"
                            ));
                        }
                        _ => unreachable!(),
                    }
                } else if h & 0xE000 == 0x2000 {
                    let op = (h >> 11) & 3;
                    let rd = ((h >> 8) & 7) as usize;
                    let imm = (h & 255) as u32;
                    match op {
                        0 => body.push_str(&format!("r[{rd}]={imm}u; z=(r[{rd}]==0); n=0;")),
                        1 => body.push_str(&format!("{{ uint32_t a=r[{rd}], b={imm}u, t=a-b; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}")),
                        2 => body.push_str(&format!("{{ uint32_t a=r[{rd}], b={imm}u, t=a+b; uint64_t wide=(uint64_t)a+b; r[{rd}]=t; z=(t==0); n=(t>>31); c=(wide>>32); v=((~(a^b)&(a^t))>>31); }}")),
                        _ => body.push_str(&format!("{{ uint32_t a=r[{rd}], b={imm}u, t=a-b; r[{rd}]=t; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}")),
                    }
                } else if h & 0xF800 == 0x2800 {
                    let rn = ((h >> 8) & 7) as usize;
                    let imm = (h & 255) as u32;
                    body.push_str(&format!("{{ uint32_t a=r[{rn}], b={imm}u, t=a-b; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}"));
                } else if h & 0xF800 == 0x1800 {
                    let immediate = h & 0x400 != 0;
                    let sub = h & 0x200 != 0;
                    let rd = (h & 7) as usize;
                    let rn = ((h >> 3) & 7) as usize;
                    let operand = if immediate {
                        format!("{}u", (h >> 6) & 7)
                    } else {
                        format!("r[{}]", (h >> 6) & 7)
                    };
                    if sub {
                        body.push_str(&format!(
                            "{{ uint32_t a=r[{rn}], b={operand}, t=a-b; r[{rd}]=t; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}"
                        ));
                    } else {
                        body.push_str(&format!(
                            "{{ uint32_t a=r[{rn}], b={operand}, t=a+b; uint64_t wide=(uint64_t)a+b; r[{rd}]=t; z=(t==0); n=(t>>31); c=(wide>>32); v=((~(a^b)&(a^t))>>31); }}"
                        ));
                    }
                } else if h & 0xF800 == 0xE000 {
                    let imm = (((h & 0x7FF) as i16) << 5 >> 4) as i32;
                    let target = addr.wrapping_add(4).wrapping_add(imm as u32);
                    pending.push((target, true));
                    body.push_str(&format!("pc = 0x{:08X}u;", target | 1));
                    changes_pc = true;
                    ended = true;
                } else if h & 0xF000 == 0xD000 && (h >> 8) & 15 < 14 {
                    let cond = (h >> 8) & 15;
                    let imm = (((h & 255) as i8 as i32) << 1) as u32;
                    let target = addr.wrapping_add(4).wrapping_add(imm);
                    pending.push((target, true));
                    condition = arm_condition_c(cond as u32).to_string();
                    body.push_str(&format!("pc = 0x{:08X}u;", target | 1));
                    changes_pc = true;
                } else if matches!(h & 0xFFF0, 0xF3C0 | 0xF340) && off + 4 <= bytes.len() {
                    let h2 = u16::from_le_bytes(bytes[off + 2..off + 4].try_into().unwrap());
                    let rn = (h & 15) as usize;
                    let rd = ((h2 >> 8) & 15) as usize;
                    let lsb = (((h2 >> 12) & 7) << 2) | ((h2 >> 6) & 3);
                    let width = (h2 & 31) + 1;
                    let mask = if width == 32 {
                        u32::MAX
                    } else {
                        (1u32 << width) - 1
                    };
                    if h & 0xFFF0 == 0xF3C0 {
                        body.push_str(&format!("r[{rd}] = (r[{rn}] >> {lsb}) & 0x{mask:08X}u;"));
                    } else {
                        let shift = 32 - width;
                        let left = 32 - lsb - width;
                        body.push_str(&format!(
                            "r[{rd}] = (uint32_t)(((int32_t)(r[{rn}] << {left})) >> {shift});"
                        ));
                    }
                    next = addr.wrapping_add(4);
                    next_key = next | 1;
                } else if matches!(h & 0xFF80, 0xEB00 | 0xEBA0) && off + 4 <= bytes.len() {
                    let h2 = u16::from_le_bytes(bytes[off + 2..off + 4].try_into().unwrap());
                    let rn = (h & 15) as usize;
                    let rd = ((h2 >> 8) & 15) as usize;
                    let rm = (h2 & 15) as usize;
                    let shift = (((h2 >> 12) & 7) << 2) | ((h2 >> 6) & 3);
                    let kind = ((h2 >> 4) & 3) as u32;
                    let sub = h & 0xFF80 == 0xEBA0;
                    let op = if sub { "-" } else { "+" };
                    body.push_str(&format!("{{ uint32_t a=r[{rn}], b=arm_shift(r[{rm}], {kind}, {shift}), t=a {op} b; r[{rd}]=t;"));
                    if h & 0x0010 != 0 {
                        if sub {
                            body.push_str(" z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31);");
                        } else {
                            body.push_str(" uint64_t wide=(uint64_t)a+b; z=(t==0); n=(t>>31); c=(wide>>32); v=((~(a^b)&(a^t))>>31);");
                        }
                    }
                    body.push_str(" }");
                    next = addr.wrapping_add(4);
                    next_key = next | 1;
                } else if h & 0xFE40 == 0xE800 && off + 4 <= bytes.len() {
                    let h2 = u16::from_le_bytes(bytes[off + 2..off + 4].try_into().unwrap());
                    let load = h & 0x0010 != 0;
                    let writeback = h & 0x0020 != 0;
                    let rn = (h & 15) as usize;
                    let list = h2;
                    if list == 0 || rn == 15 {
                        return Err(format!("invalid Thumb-2 block transfer at 0x{addr:08X}"));
                    }
                    let count = list.count_ones();
                    body.push_str(&format!("{{ uint32_t base=r[{rn}];"));
                    let mut index = 0u32;
                    for reg in 0..16 {
                        if list & (1 << reg) != 0 {
                            if load {
                                body.push_str(&format!("r[{reg}]=mem32(base + {}u);", index * 4));
                            } else {
                                body.push_str(&format!("put32(base + {}u, r[{reg}]);", index * 4));
                            }
                            index += 1;
                        }
                    }
                    if writeback && (!load || list & (1 << rn) == 0) {
                        body.push_str(&format!("r[{rn}]=base + {}u;", count * 4));
                    }
                    body.push_str(" }");
                    next = addr.wrapping_add(4);
                    next_key = next | 1;
                } else if h & 0xF800 == 0xF000 && off + 4 <= bytes.len() {
                    let h2 = u16::from_le_bytes(bytes[off + 2..off + 4].try_into().unwrap());
                    if h2 & 0xD000 != 0xD000 && h2 & 0xD000 != 0xC000 {
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{:08X}u, 1u);",
                            ((h as u32) << 16) | h2 as u32
                        ));
                        ended = true;
                    }
                    let (target, blx_arm) = thumb_bl_target(addr, h, h2).ok_or_else(|| {
                        format!("unsupported Thumb-2 branch at 0x{addr:08X}: {h:04X} {h2:04X}")
                    })?;
                    if target >= base && target as usize + 2 <= base as usize + bytes.len() {
                        enqueue_known_seed(&mut pending, &mut force_a32, target, !blx_arm);
                    }
                    next = addr.wrapping_add(4);
                    next_key = next | 1;
                    body.push_str(&format!(
                        "r[14] = 0x{next_key:08X}u; pc = 0x{:08X}u;",
                        target | u32::from(!blx_arm)
                    ));
                    changes_pc = true;
                    if noreturn_calls.contains(&(target & !1)) {
                        ended = true;
                    }
                } else if h & 0xF000 == 0x9000 {
                    let load = h & 0x800 != 0;
                    let rd = ((h >> 8) & 7) as usize;
                    let off = ((h & 255) * 4) as u32;
                    if load {
                        body.push_str(&format!("r[{rd}] = mem32(r[13] + {off}u);"));
                    } else {
                        body.push_str(&format!("put32(r[13] + {off}u, r[{rd}]);"));
                    }
                } else if h & 0xF800 == 0x4800 {
                    let rd = ((h >> 8) & 7) as usize;
                    let off = ((h & 255) * 4) as u32;
                    let literal_base = addr.wrapping_add(4) & !3;
                    body.push_str(&format!(
                        "r[{rd}] = mem32(0x{:08X}u + {off}u);",
                        literal_base
                    ));
                } else if h & 0xF000 == 0xA000 {
                    let rd = ((h >> 8) & 7) as usize;
                    let off = ((h & 255) * 4) as u32;
                    let base_expr = if h & 0x0800 != 0 {
                        "r[13]".to_string()
                    } else {
                        format!("0x{:08X}u", addr.wrapping_add(4) & !3)
                    };
                    body.push_str(&format!("r[{rd}] = {base_expr} + {off}u;"));
                } else if h & 0xFC00 == 0x4400 {
                    let op = (h >> 8) & 3;
                    let rd = ((h & 7) | ((h >> 4) & 8)) as usize;
                    let rm = ((h >> 3) & 15) as usize;
                    match op {
                        0 => {
                            if rd == 15 {
                                let operand = if rm == 15 {
                                    format!("0x{:08X}u", addr.wrapping_add(4))
                                } else {
                                    format!("r[{rm}]")
                                };
                                body.push_str(&format!("pc = branch_indirect({operand});"));
                                changes_pc = true;
                                ended = true;
                            } else if rm == 15 {
                                body.push_str(&format!(
                                    "r[{rd}] += 0x{:08X}u;",
                                    addr.wrapping_add(4)
                                ));
                            } else {
                                body.push_str(&format!("r[{rd}] += r[{rm}];"));
                            }
                        }
                        1 => {
                            let rhs = if rm == 15 {
                                format!("0x{:08X}u", addr.wrapping_add(4))
                            } else {
                                format!("r[{rm}]")
                            };
                            body.push_str(&format!("{{ uint32_t a=r[{rd}], b={rhs}, t=a-b; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}"));
                        }
                        2 => {
                            if rd == 15 {
                                if rm == 15 {
                                    body.push_str(&format!(
                                        "pc = 0x{:08X}u;",
                                        addr.wrapping_add(4)
                                    ));
                                } else {
                                    body.push_str(&format!("pc = branch_indirect(r[{rm}]);"));
                                }
                                changes_pc = true;
                                ended = true;
                            } else if rm == 15 {
                                body.push_str(&format!(
                                    "r[{rd}] = 0x{:08X}u;",
                                    addr.wrapping_add(4)
                                ));
                            } else {
                                body.push_str(&format!("r[{rd}] = r[{rm}];"));
                            }
                        }
                        3 => {
                            let link = h & 0x0080 != 0;
                            if link {
                                if addr == 0x0010_33EC && rm == 0 {
                                    // The startup's relative constructor table is mutable while
                                    // the ARM11 loader/initializers are running. Ignore entries
                                    // that have already become non-code pointers after relocation.
                                    body.push_str(&format!(
                                        "r[14] = 0x{next_key:08X}u; pc = (r[0] >= 0x00100000u && r[0] < 0x00600000u) ? branch_indirect(r[0]) : 0x{next_key:08X}u;"
                                    ));
                                } else {
                                    body.push_str(&format!(
                                        "r[14] = 0x{next_key:08X}u; pc = branch_indirect(r[{rm}]);"
                                    ));
                                }
                            } else if rm == 14 {
                                body.push_str(
                                    "if (r[14] == 0xFFFFFFFFu) running = 0; else pc = r[14];",
                                );
                            } else {
                                body.push_str(&format!("pc = branch_indirect(r[{rm}]);"));
                            }
                            changes_pc = true;
                            ended = !link;
                        }
                        _ => unreachable!(),
                    }
                } else if h & 0xF000 == 0x5000 {
                    let op = (h >> 9) & 7;
                    let ro = ((h >> 6) & 7) as usize;
                    let rn = ((h >> 3) & 7) as usize;
                    let rd = (h & 7) as usize;
                    let ea = format!("(r[{rn}] + r[{ro}])");
                    match op {
                        0 => body.push_str(&format!("put32({ea}, r[{rd}]);")),
                        1 => body.push_str(&format!("put16({ea}, r[{rd}]);")),
                        2 => body.push_str(&format!("put8({ea}, r[{rd}]);")),
                        3 => body
                            .push_str(&format!("r[{rd}] = (uint32_t)(int32_t)(int8_t)mem8({ea});")),
                        4 => body.push_str(&format!("r[{rd}] = mem32({ea});")),
                        5 => body.push_str(&format!("r[{rd}] = mem16({ea});")),
                        6 => body.push_str(&format!("r[{rd}] = mem8({ea});")),
                        _ => body.push_str(&format!(
                            "r[{rd}] = (uint32_t)(int32_t)(int16_t)mem16({ea});"
                        )),
                    }
                } else if h & 0xE000 == 0x6000 {
                    let op = (h >> 11) & 3;
                    let imm = ((h >> 6) & 31) as u32;
                    let rn = ((h >> 3) & 7) as usize;
                    let rd = (h & 7) as usize;
                    let byte = op >= 2;
                    let off = imm * if byte { 1 } else { 4 };
                    let ea = format!("(r[{rn}] + {off}u)");
                    match op {
                        0 => body.push_str(&format!("put32({ea}, r[{rd}]);")),
                        1 => body.push_str(&format!("r[{rd}] = mem32({ea});")),
                        2 => body.push_str(&format!("put8({ea}, r[{rd}]);")),
                        _ => body.push_str(&format!("r[{rd}] = mem8({ea});")),
                    }
                } else if h & 0xF000 == 0xC000 {
                    let load = h & 0x0800 != 0;
                    let rn = ((h >> 8) & 7) as usize;
                    let list = h & 255;
                    if list == 0 {
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{h:04X}u, 1u);"
                        ));
                        changes_pc = true;
                        ended = true;
                    }
                    let count = list.count_ones();
                    body.push_str(&format!("{{ uint32_t base=r[{rn}];"));
                    let mut index = 0u32;
                    for reg in 0..8 {
                        if list & (1 << reg) != 0 {
                            if load {
                                body.push_str(&format!("r[{reg}]=mem32(base + {}u);", index * 4));
                            } else {
                                body.push_str(&format!("put32(base + {}u, r[{reg}]);", index * 4));
                            }
                            index += 1;
                        }
                    }
                    if !load || list & (1 << rn) == 0 {
                        body.push_str(&format!("r[{rn}]=base + {}u;", count * 4));
                    }
                    body.push_str(" }");
                } else if h & 0xF000 == 0x8000 {
                    let load = h & 0x0800 != 0;
                    let off = (((h >> 6) & 31) * 2) as u32;
                    let rn = ((h >> 3) & 7) as usize;
                    let rd = (h & 7) as usize;
                    let ea = format!("(r[{rn}] + {off}u)");
                    if load {
                        body.push_str(&format!("r[{rd}] = mem16({ea});"));
                    } else {
                        body.push_str(&format!("put16({ea}, r[{rd}]);"));
                    }
                } else if h & 0xFF00 == 0x4700 {
                    let rm = ((h >> 3) & 15) as usize;
                    let link = h & 0x80 != 0;
                    if link {
                        body.push_str(&format!(
                            "r[14] = 0x{:08X}u; pc = branch_indirect(r[{rm}]);",
                            next_key
                        ));
                    } else {
                        body.push_str(&format!("pc = branch_indirect(r[{rm}]);"));
                    }
                    changes_pc = true;
                    ended = !link;
                } else if (0xEC00..=0xEFFF).contains(&h) && off + 4 <= bytes.len() {
                    // Thumb-2 coprocessor space: after the halfword swap this is
                    // A32 bits[31:16] = 0xEC..-0xEF.., i.e. the VFP transfer
                    // (0xEC/0xED) and VFP data-processing (0xEE/0xEF) families.
                    // The coprocessor number carried in the second halfword
                    // selects VFP (10/11); Advanced SIMD lives beside it and has
                    // no arm anywhere in this recompiler.  This was the Thumb
                    // path's whole class gap: hw1 0xED8D x2, 0xEC80, 0xEEB4 and
                    // 0xEF68 all used to fall through to the final reject (or to
                    // the A32-middle stop) - sweep t_4734a0a2, 5 sites.
                    let h2 = u16::from_le_bytes(bytes[off + 2..off + 4].try_into().unwrap());
                    let a32 = ((h as u32) << 16) | h2 as u32;
                    let cp = (h2 >> 8) & 15;
                    if !matches!(cp, 10 | 11) {
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{a32:08X}u, 1u);"
                        ));
                        changes_pc = true;
                        ended = true;
                    } else if ((a32 >> 16) & 15) == 15 && ((a32 >> 25) & 7) == 6 {
                        // PC-relative VFP literal: the A32 arm takes its base as
                        // addr+8, Thumb-2 aligns addr+4.  DQ8.code contains no
                        // such instruction (whole-image sweep), so reject loudly
                        // rather than pick a base (R2-01).
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{a32:08X}u, 1u);"
                        ));
                        changes_pc = true;
                        ended = true;
                    } else {
                        // Delegate: record the reconstructed word for this
                        // instruction and for any adjacent Thumb-2 VFP run, then
                        // queue ONE forced A32 walk that decodes the run through
                        // the existing VFP chain.  The sentinel after the run
                        // stops that walk (one 4-byte step past the last member)
                        // and hands control back to Thumb state at probe|1.
                        let mut probe = addr;
                        let mut run_units = 0u32;
                        while run_units < 64 {
                            let poff = (probe - base) as usize;
                            if poff + 4 > bytes.len() {
                                break;
                            }
                            let ph1 =
                                u16::from_le_bytes(bytes[poff..poff + 2].try_into().unwrap());
                            let ph2 = u16::from_le_bytes(
                                bytes[poff + 2..poff + 4].try_into().unwrap(),
                            );
                            if !(0xEC00..=0xEFFF).contains(&ph1)
                                || !matches!((ph2 >> 8) & 15, 10 | 11)
                            {
                                break;
                            }
                            a32_word.insert(probe, ((ph1 as u32) << 16) | ph2 as u32);
                            probe = probe.wrapping_add(4);
                            run_units += 1;
                        }
                        force_a32.insert(addr);
                        pending.push((addr, false));
                        if !emitted.contains_key(&probe) {
                            let back = probe | 1;
                            // Unconditional redirect: the sentinel hands control
                            // back to Thumb state at `back`, so there is no
                            // second (unreachable) fall-through arm to emit.
                            emitted.insert(probe, format!("pc = 0x{back:08X}u;"));
                        }
                        // This Thumb case only redirects into its A32 twin; the
                        // Thumb walk itself continues, so the case at (addr+4)|1
                        // exists for the sentinel to return to.
                        body.push_str(&format!("pc = 0x{addr:08X};"));
                        changes_pc = true;
                        ended = false;
                    }
                    next = addr.wrapping_add(4);
                    next_key = next | 1;
                } else if looks_like_a32_instruction_middle(&bytes, base, addr) {
                    // A conservative pointer scan occasionally reaches the
                    // second half of an A32 word as if it were Thumb code.
                    // Keep the case explicit and stop this false path.
                    body.push_str("pc = 0xFFFFFFFFu; running = 0;");
                    changes_pc = true;
                    ended = true;
                } else {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{h:04X}u, 1u);"
                    ));
                    changes_pc = true;
                    ended = true;
                }
                if !changes_pc {
                    body.push_str(&format!("pc = 0x{next_key:08X}u;"));
                }
                emitted.insert(
                    pc_key,
                    condition_wrap(
                        &condition,
                        &body,
                        &format!("pc = 0x{next_key:08X}u;"),
                    ),
                );
                total_insns += 1;
                if ended {
                    break;
                }
                addr = next;
                continue;
            }
            let off = (addr - base) as usize;
            if off + 4 > bytes.len() {
                break;
            }
            let mut w = u32::from_le_bytes(bytes[off..off + 4].try_into().unwrap());
            if let Some(reconstructed) = a32_word.remove(&addr) {
                // Thumb-2 coprocessor instruction being re-decoded in A32
                // state: the bytes at `addr` are its swapped halves, so decode
                // the reconstructed A32-equivalent word instead (see the
                // `a32_word` declaration).
                w = reconstructed;
            }
            let cond = w >> 28;
            // ARM condition 0xE is AL; 0xF is reserved for encodings that
            // this lifter treats as unconditional extensions/hints.
            let unconditional = cond >= 14;
            let mut body = String::new();
            let mut changes_pc = false;
            if w & 0x0FFF_0FFF == 0x0EE1_0A10 {
                let rt = ((w >> 12) & 15) as usize;
                body.push_str(&format!("fpscr = r[{rt}];"));
            } else if w & 0x0FFF_0FFF == 0x0EF1_0A10 {
                let rt = ((w >> 12) & 15) as usize;
                if rt == 15 {
                    body.push_str(
                        "n=(fpscr>>31)&1; z=(fpscr>>30)&1; c=(fpscr>>29)&1; v=(fpscr>>28)&1;",
                    );
                } else {
                    body.push_str(&format!("r[{rt}] = fpscr;"));
                }
            } else if w & 0x0FFF_0FFF == 0x0EE8_0A10 {
                // VMSR FPEXC, Rt (legacy FMXR FPEXC is the same encoding).
                // Round-17 sibling audit of the VMRS/VMSR family: the FPSCR
                // moves above were already handled; FPEXC was not and fell
                // through to guest_unsupported.  This runtime models VFP as
                // permanently enabled - there is no enable bit to store and
                // no VFP-enable trap path to honour - so the write is
                // accepted and dropped.  The drop is recorded in the emitted
                // C instead of being left as a bare empty statement.
                body.push_str("/* VMSR FPEXC: VFP permanently enabled here, write dropped */");
            } else if w & 0x0FFF_0FFF == 0x0EF8_0A10 {
                // VMRS Rt, FPEXC (legacy FMRX): EN (bit 30) reads as set
                // because VFP is always enabled in this runtime; no other
                // FPEXC state exists to expose.  ARM makes VMRS to r15
                // unpredictable for FPEXC, so reject loudly rather than
                // invent APSR semantics (R2-01: no silent lowering).
                let rt = ((w >> 12) & 15) as usize;
                if rt == 15 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else {
                    body.push_str(&format!("r[{rt}] = 0x40000000u;"));
                }
            } else if matches!(
                w & 0x0FBF_0ED0,
                0x0EB4_0A40 | 0x0EB4_0AC0 | 0x0EB5_0A40 | 0x0EB5_0AC0
            ) {
                // VFP VCMP/VCMPE. The FPSCR NZCV nibble uses ARM's unordered
                // comparison encoding: 0011 for NaN, 0110 equal, 1000 less,
                // and 0010 greater.
                let double = w & (1 << 8) != 0;
                let zero = w & (1 << 16) != 0;
                let d = if double {
                    ((((w >> 12) & 15) + (((w >> 22) & 1) * 16)) * 2) as usize
                } else {
                    (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize
                };
                let m = if double {
                    (((w & 15) + (((w >> 5) & 1) * 16)) * 2) as usize
                } else {
                    ((w & 15) * 2 + ((w >> 5) & 1)) as usize
                };
                let (a_bits, b_bits, a, b, nan_test) = if double {
                    let rhs = if zero {
                        "0ull".to_string()
                    } else {
                        format!("(((uint64_t)fpr[{}] << 32) | fpr[{}])", m + 1, m)
                    };
                    (
                        format!("(((uint64_t)fpr[{}] << 32) | fpr[{}])", d + 1, d),
                        rhs,
                        format!("f64_from_bits(_a_bits)"),
                        format!("f64_from_bits(_b_bits)"),
                        "f64_is_nan_bits(_a_bits) || f64_is_nan_bits(_b_bits)".to_string(),
                    )
                } else {
                    let rhs = if zero {
                        "0u".to_string()
                    } else {
                        format!("fpr[{m}]")
                    };
                    (
                        format!("fpr[{d}]"),
                        rhs,
                        "f32_from_bits(_a_bits)".to_string(),
                        "f32_from_bits(_b_bits)".to_string(),
                        "f32_is_nan_bits(_a_bits) || f32_is_nan_bits(_b_bits)".to_string(),
                    )
                };
                body.push_str(&format!(
                    "{{ uint64_t _a_bits={a_bits}, _b_bits={b_bits}; double _a={a}, _b={b}; uint32_t _flags={nan_test} ? 0x30000000u : (_a == _b ? 0x60000000u : (_a < _b ? 0x80000000u : 0x20000000u)); fpscr=(fpscr & 0x0FFFFFFFu) | _flags; }}"
                ));
            } else if w & 0x0FBF_0FD0 == 0x0EB0_0A40 {
                // VFP VMOV.F32 Sd, Sm (register-to-register copy).
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                body.push_str(&format!("fpr[{d}] = fpr[{m}];"));
            } else if w & 0x0FBF_0FD0 == 0x0EB0_0B40 {
                // VFP VMOV.F64 Dd, Dm (double-precision register copy): the
                // sz bit (bit 8) is set, so this is the F64 twin of the F32
                // VMOV above and must copy BOTH words of the D-register pair.
                // Reached once the callback-escape chain was closed - esc19
                // aborted at pc=0x002017E8 on 0xEEB08B40 (vmov.f64 d8, d0).
                // vfp_double_slots bounds-checks the 16-double bank behind
                // fpr[32] (d16+ would alias fpscr) and rejects loudly (R2-01).
                match vfp_double_slots(w) {
                    Some((d, _n, m)) => {
                        body.push_str(&format!(
                            "fpr[{d}] = fpr[{m}]; fpr[{d2}] = fpr[{m2}];",
                            d = d,
                            m = m,
                            d2 = d + 1,
                            m2 = m + 1
                        ));
                    }
                    None => {
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                        ));
                        changes_pc = true;
                        ended = unconditional;
                    }
                }
            } else if w & 0x0FBF_0FD0 == 0x0EB0_0AC0 {
                // VFP VABS.F32 clears the sign bit, preserving NaN payloads.
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                body.push_str(&format!("fpr[{d}] = fpr[{m}] & 0x7FFFFFFFu;"));
            } else if w & 0x0FBF_0FD0 == 0x0EB1_0A40 {
                // VFP VNEG.F32 Sd, Sm.
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                body.push_str(&format!(
                    "fpr[{d}] = f32_to_bits(-f32_from_bits(fpr[{m}]));"
                ));
            } else if matches!(w & 0x0FBF_0FD0, 0x0EB1_0AC0 | 0x0EB1_0BC0) {
                // VFP VSQRT in single or double precision.
                let double = w & 0x0000_0100 != 0;
                if double {
                    let d = ((((w >> 12) & 15) | (((w >> 22) & 1) << 4)) * 2) as usize;
                    let m = (((w & 15) | (((w >> 5) & 1) << 4)) * 2) as usize;
                    // _src must read the SOURCE pair (m, m+1): using d+1 for
                    // the high word silently squared the wrong operand when
                    // Vd != Vm (the d0,d0 case masked the mixup).
                    body.push_str(&format!(
                        "{{ uint64_t _src=((uint64_t)fpr[{}] << 32) | fpr[{m}]; uint64_t _result=f64_to_bits(sqrt(f64_from_bits(_src))); fpr[{d}]=(uint32_t)_result; fpr[{}]=(uint32_t)(_result >> 32); }}",
                        m + 1,
                        d + 1
                    ));
                } else {
                    let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                    let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                    body.push_str(&format!(
                        "fpr[{d}] = f32_to_bits(sqrtf(f32_from_bits(fpr[{m}])));"
                    ));
                }
            } else if w & 0x0FBF_0ED0 == 0x0EB7_0AC0 {
                // VFP VCVT between the two floating-point formats.  This is
                // the round-17 wall: 0xEEB70BC0 at pc=0x00269C64 was filed as
                // "VMRS", but llvm-objdump decodes it as `vcvt.f32.f64 s0,
                // d0` (the real VMRS encoding 0xEEF10A10 is handled at the
                // top of this chain).  Field map, verified against llvm-mc
                // output for register values 0..31:
                //   bits[19:16] = 0111  - fp<->fp marker.  The fixed-point
                //     VCVT forms carry 0b1xxx here and the integer forms
                //     0b1000/0b1100/0b1101, so those keep their own arms;
                //   sz (bit 8)  - 1: to-single, source is the Dm pair;
                //                 0: to-double, source is Sm;
                //   Vd = bits[15:12] with D (bit 22) as the high bit,
                //   Vm = bits[3:0]   with M (bit 5) as the high bit.
                // The mask ignores bit 8 (direction) and bit 5 (M), and this
                // arm shares the VCMP mask shape, whose values differ in
                // bits[19:16] (0100/0101 vs 0111), so no other VFP arm can
                // claim these opcodes.  Rounding is the host default
                // (round-to-nearest), the same approximation every other VFP
                // arm above uses - FPSCR.RMode is not plumbed into these
                // conversions.  The float<->double value conversion itself is
                // exact IEEE, matching VFP with FPSCR.DN = 0 (reset value).
                if w & (1 << 8) != 0 {
                    // VCVT.F32.F64 Sd, Dm: the single destination always
                    // sits inside the 32-word fpr bank, the source pair does
                    // not when Dm >= d16 (loud reject - R2-01, the d16 bank
                    // behind fpr[32] would alias fpscr).
                    let dm = ((w & 15) | (((w >> 5) & 1) << 4)) as usize;
                    let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                    if dm >= 16 {
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                        ));
                        changes_pc = true;
                        ended = unconditional;
                    } else {
                        let m = dm * 2;
                        body.push_str(&format!(
                            "fpr[{d}] = f32_to_bits((float)f64_from_bits(((uint64_t)fpr[{h}] << 32) | fpr[{l}]));",
                            d = d,
                            h = m + 1,
                            l = m
                        ));
                    }
                } else {
                    // VCVT.F64.F32 Dd, Sm: source is a single word (always
                    // in bank), destination pair is bounds-checked.
                    let dd = (((w >> 12) & 15) | (((w >> 22) & 1) << 4)) as usize;
                    let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                    if dd >= 16 {
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                        ));
                        changes_pc = true;
                        ended = unconditional;
                    } else {
                        body.push_str(&vfp_f64_store(
                            dd * 2,
                            &format!("(double)f32_from_bits(fpr[{m}])"),
                        ));
                    }
                }
            } else if matches!(w & 0x0FBF_0FD0, 0x0EB8_0BC0 | 0x0EB8_0B40) {
                // VCVT.F64.S32 / VCVT.F64.U32 Dd, Sm: the double-precision
                // twins of the single-precision integer->float conversions
                // further down (sz/bit 8 = 1; bit 7 selects signed vs
                // unsigned exactly as in those single forms).  Out-of-bank
                // destinations reject loudly, and the integer->float casts
                // mirror the single arms so both precisions round alike.
                let dd = (((w >> 12) & 15) | (((w >> 22) & 1) << 4)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                if dd >= 16 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else {
                    let conv = if w & (1 << 7) != 0 {
                        format!("(double)(int32_t)fpr[{m}]")
                    } else {
                        format!("(double)(uint32_t)fpr[{m}]")
                    };
                    body.push_str(&vfp_f64_store(dd * 2, &conv));
                }
            } else if matches!(w & 0x0FBF_0FD0, 0x0EBD_0BC0 | 0x0EBC_0BC0) {
                // VCVT.S32.F64 / VCVT.U32.F64 Sd, Dm: double->integer twins
                // of the single-precision forms below (bit 16 selects signed
                // vs unsigned, 0b1101 vs 0b1100 in bits[19:16]).  Plain VCVT
                // rounds toward zero - VCVTR would take FPSCR.RMode instead
                // and this guest does not emit it - which is what a C cast
                // does, matching the single-precision arms.
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let dm = ((w & 15) | (((w >> 5) & 1) << 4)) as usize;
                if dm >= 16 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else {
                    let src = format!(
                        "f64_from_bits(((uint64_t)fpr[{h}] << 32) | fpr[{l}])",
                        h = dm * 2 + 1,
                        l = dm * 2
                    );
                    let conv = if w & (1 << 16) != 0 {
                        format!("(uint32_t)(int32_t){src}")
                    } else {
                        format!("(uint32_t){src}")
                    };
                    body.push_str(&format!("fpr[{d}] = {conv};"));
                }
            } else if w & 0x0FE0_0FD0 == 0x0C40_0B10 {
                // VFP VMOV between the ARM core and a DOUBLE register
                // (two-register form): `vmov Rt, Rt2, Dm` / `vmov Dd, Rt,
                // Rt2` - the MRRC/MCRR coproc-11 twin of the single-register
                // VMOV arm below (0x0E00_0A10).  Round-17 audit found lifted
                // sites still rejecting: 0xEC532B10 (@0x0016F5E8) and
                // 0xEC510B18 (@0x002FFDC0).  Field map from llvm-mc for
                // d0/d1/d8/d16/d17/d30 crossed with r0-r5: Rt = bits[15:12],
                // Rt2 = bits[19:16], the D register is M(bit5):Vm(bits[3:0])
                // like every other VFP form, bit4 = 1 fixed, and bit 20 (L)
                // picks the direction (1 = VFP -> core, 0 = core -> VFP).
                // Rt takes the LOW word of the double, Rt2 the HIGH word.
                let reg = (((w & 15) | (((w >> 5) & 1) << 4))) as usize;
                let rt = ((w >> 12) & 15) as usize;
                let rt2 = ((w >> 16) & 15) as usize;
                if reg >= 16 {
                    // d16+ sits behind fpr[32] (it would alias fpscr) - loud
                    // reject, never a silent neighbouring-bank access (R2-01).
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else if w & (1 << 20) != 0 {
                    body.push_str(&format!(
                        "r[{rt}] = fpr[{lo}]; r[{rt2}] = fpr[{hi}];",
                        rt = rt,
                        rt2 = rt2,
                        lo = reg * 2,
                        hi = reg * 2 + 1
                    ));
                } else {
                    body.push_str(&format!(
                        "fpr[{lo}] = r[{rt}]; fpr[{hi}] = r[{rt2}];",
                        rt = rt,
                        rt2 = rt2,
                        lo = reg * 2,
                        hi = reg * 2 + 1
                    ));
                }
            } else if w & 0x0FE0_0F7F == 0x0E00_0A10 {
                let arm = ((w >> 12) & 15) as usize;
                let s = (((w >> 16) & 15) * 2 + ((w >> 7) & 1)) as usize;
                if w & (1 << 20) != 0 {
                    body.push_str(&format!("r[{arm}] = fpr[{s}];"));
                } else {
                    body.push_str(&format!("fpr[{s}] = r[{arm}];"));
                }
            } else if w & 0x0FBF_0FD0 == 0x0EB8_0AC0 {
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                body.push_str(&format!(
                    "fpr[{d}] = f32_to_bits((float)(int32_t)fpr[{m}]);"
                ));
            } else if w & 0x0FBF_0FD0 == 0x0EB8_0A40 {
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                body.push_str(&format!(
                    "fpr[{d}] = f32_to_bits((float)(uint32_t)fpr[{m}]);"
                ));
            } else if w & 0x0FBF_0FD0 == 0x0EBD_0AC0 {
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                body.push_str(&format!(
                    "fpr[{d}] = (uint32_t)(int32_t)f32_from_bits(fpr[{m}]);"
                ));
            } else if w & 0x0FBF_0FD0 == 0x0EBC_0AC0 {
                let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                body.push_str(&format!("fpr[{d}] = (uint32_t)f32_from_bits(fpr[{m}]);"));
            } else if matches!(w & 0x0FB0_0E50, 0x0E10_0A00 | 0x0E10_0A40) {
                // VFP negated multiply-accumulate forms used by DQ8's
                // floating-point formatter. Bit 6 selects VNMLS vs VNMLA;
                // bit 8 (sz) selects double vs single precision, matching
                // azahar's vfpinstr.cpp double/single dispatch on BIT(inst,8).
                // F64 forms must use the D-register pair path - silently
                // reinterpreting the operands as F32 produced wrong results
                // for ordinary double arithmetic (audit R2-01).
                let sign = if w & 0x40 == 0 { "-" } else { "+" };
                if w & 0x0000_0100 != 0 {
                    match vfp_double_slots(w) {
                        Some((d, n, m)) => {
                            let expr = format!(
                                "-({d} {sign} {n} * {m})",
                                d = vfp_f64_read(d),
                                n = vfp_f64_read(n),
                                m = vfp_f64_read(m),
                                sign = sign
                            );
                            body.push_str(&vfp_f64_store(d, &expr));
                        }
                        None => {
                            body.push_str(&format!(
                                "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                            ));
                            changes_pc = true;
                            ended = unconditional;
                        }
                    }
                } else {
                    let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                    let n = (((w >> 16) & 15) * 2 + ((w >> 7) & 1)) as usize;
                    let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                    let expr = format!(
                        "-({d} {sign} {n} * {m})",
                        d = format!("f32_from_bits(fpr[{d}])"),
                        n = format!("f32_from_bits(fpr[{n}])"),
                        m = format!("f32_from_bits(fpr[{m}])"),
                        sign = sign
                    );
                    body.push_str(&format!("fpr[{d}] = f32_to_bits({expr});"));
                }
            } else if matches!(
                w & 0x0FB0_0E50,
                0x0E00_0A00
                    | 0x0E00_0A40
                    | 0x0E20_0A00
                    | 0x0E20_0A40
                    | 0x0E30_0A00
                    | 0x0E30_0A40
                    | 0x0E80_0A00
            ) {
                let masked = w & 0x0FB0_0E50;
                let accumulate = matches!(masked, 0x0E00_0A00 | 0x0E00_0A40);
                let op = match masked {
                    0x0E00_0A00 => "+",
                    0x0E00_0A40 => "-",
                    0x0E20_0A40 => "neg*",
                    0x0E30_0A00 => "+",
                    0x0E30_0A40 => "-",
                    0x0E80_0A00 => "/",
                    _ => "*",
                };
                if w & 0x0000_0100 != 0 {
                    // F64: bit 8 (sz) is the only difference from the F32
                    // encoding (llvm-mc: same op, .f64); the operands are
                    // D-register word pairs, not single fpr words.  The old
                    // mask ignored bit 8, so VADD.F64 etc. executed as F32
                    // and never computed the high word (audit R2-01).
                    match vfp_double_slots(w) {
                        Some((d, n, m)) => {
                            let expr = if accumulate {
                                let sign = if w & 0x40 == 0 { "+" } else { "-" };
                                format!(
                                    "{d} {sign} {n} * {m}",
                                    d = vfp_f64_read(d),
                                    n = vfp_f64_read(n),
                                    m = vfp_f64_read(m),
                                    sign = sign
                                )
                            } else if op == "neg*" {
                                format!(
                                    "-{n} * {m}",
                                    n = vfp_f64_read(n),
                                    m = vfp_f64_read(m)
                                )
                            } else {
                                format!(
                                    "{n} {op} {m}",
                                    n = vfp_f64_read(n),
                                    op = op,
                                    m = vfp_f64_read(m)
                                )
                            };
                            body.push_str(&vfp_f64_store(d, &expr));
                        }
                        None => {
                            body.push_str(&format!(
                                "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                            ));
                            changes_pc = true;
                            ended = unconditional;
                        }
                    }
                } else {
                    let d = (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize;
                    let n = (((w >> 16) & 15) * 2 + ((w >> 7) & 1)) as usize;
                    let m = ((w & 15) * 2 + ((w >> 5) & 1)) as usize;
                    if !accumulate {
                        match op {
                        "+" | "-" => body.push_str(&format!(
                            "fpr[{d}] = f32_to_bits(f32_from_bits(fpr[{n}]) {op} f32_from_bits(fpr[{m}]));"
                        )),
                        "neg*" => body.push_str(&format!(
                            "fpr[{d}] = f32_to_bits(-f32_from_bits(fpr[{n}]) * f32_from_bits(fpr[{m}]));"
                        )),
                        _ => body.push_str(&format!(
                            "fpr[{d}] = f32_to_bits(f32_from_bits(fpr[{n}]) {op} f32_from_bits(fpr[{m}]));"
                        )),
                        }
                    } else {
                        let sign = if w & 0x40 == 0 { "+" } else { "-" };
                        body.push_str(&format!(
                            " fpr[{d}] = f32_to_bits(f32_from_bits(fpr[{d}]) {sign} f32_from_bits(fpr[{n}]) * f32_from_bits(fpr[{m}]));"
                        ));
                    }
                }
            } else if w & 0x0E00_0E00 == 0x0C00_0A00
                && matches!((w >> 8) & 15, 10 | 11)
                && (w & (1 << 24) != 0) != (w & (1 << 23) != 0)
                // W (bit 21) and P (bit 24) together separate the two families
                // sharing this encoding space, per llvm-mc ground truth:
                //   * `vstmia r0, {s0-s2}` (no writeback) = 0xEC800A03 -
                //     P=0, U=1, W=0 - assembles and decodes as a multiple
                //     transfer, so W=0 is NOT by itself an immediate form;
                //   * `vstmdb r0, {s0-s2}` WITHOUT `!` is rejected as an
                //     invalid instruction - the pre-indexed (P=1, U=0)
                //     multiple form always writes back, so P=1 && W=0 can
                //     only be the immediate VLDR/VSTR form;
                //   * vpush/vpop/vstmia! all have P!=U with W=1, and
                //     `vstr s0, [r0, #-12]` = 0xED000A03 has P=1, W=0.
                // Without the P=1 && W=0 exclusion a NEGATIVE-offset VSTR
                // (P=1, U=0, W=0) satisfies P != U and lands here, where imm8
                // is read as a transfer COUNT instead of an offset - esc30
                // aborted at pc=0x00220138 on 0xED408ACA (`vstr s17, [r0,
                // #-808]`, imm8=202 > 32) and a smaller negative offset
                // (imm8 <= 32) would have been silently executed as a
                // multi-word store instead.
                && ((w & (1 << 24) == 0) || (w & (1 << 21) != 0))
                && (w & 255) != 0
                && ((w >> 16) & 15) != 15
            {
                // VFP single/double register multiple transfers. D registers
                // occupy adjacent S-register slots in the runner state.
                let double = (w >> 8) & 15 == 11;
                let pre = w & (1 << 24) != 0;
                let up = w & (1 << 23) != 0;
                let writeback = w & (1 << 21) != 0;
                let load = w & (1 << 20) != 0;
                let rn = ((w >> 16) & 15) as usize;
                let first = if double {
                    ((((w >> 12) & 15) | (((w >> 22) & 1) << 4)) * 2) as usize
                } else {
                    (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize
                };
                let count = (w & 255) as usize;
                // imm8 is the transfer size in 32-bit words for both S- and
                // D-register forms. A D register consumes two of those words;
                // multiplying the count here double-counts VPush/VPop D lists.
                let registers = count;
                if count == 0
                    || registers > 32
                    || (double && (count & 1) != 0)
                    || (rn == 15 && writeback)
                {
                    // Architecturally invalid list, or a word that merely
                    // resembles a VFP multiple transfer (e.g. literal-pool
                    // data reached by linear decode after a branch). Per the
                    // design note on this decoder, unsupported encodings stop
                    // the translation of this block instead of aborting the
                    // whole generation - failing here made it impossible to
                    // add recomp roots for any function whose decode reaches
                    // such a word.
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = true;
                } else {
                let bytes = registers * 4;
                let base_expr = if rn == 15 {
                    format!("0x{:08X}u", addr + 8)
                } else {
                    format!("r[{rn}]")
                };
                let start_expr = if up {
                    if pre {
                        format!("{base_expr} + 4u")
                    } else {
                        base_expr.clone()
                    }
                } else if pre {
                    format!("{base_expr} - {bytes}u")
                } else {
                    format!("{base_expr} - {}u", bytes - 4)
                };
                body.push_str(&format!("{{ uint32_t a={start_expr};"));
                for i in 0..registers {
                    // The VFP multiple-transfer encoding advances through the
                    // architectural S register file modulo 32.
                    let s = (first + i) % 32;
                    let off = i * 4;
                    if load {
                        body.push_str(&format!("fpr[{s}] = mem32(a + {off}u);"));
                    } else {
                        body.push_str(&format!("put32(a + {off}u, fpr[{s}]);"));
                    }
                }
                if writeback && rn != 15 {
                    body.push_str(&format!(
                        " r[{rn}] = {}u;",
                        if up {
                            format!("r[{rn}] + {bytes}")
                        } else {
                            format!("r[{rn}] - {bytes}")
                        }
                    ));
                }
                body.push_str(" }");
                }
            } else if w & 0x0F00_0E00 == 0x0D00_0A00 && (w >> 8) & 15 >= 10 && (w >> 8) & 15 <= 11 {
                // VFP VLDR/VSTR immediate. coproc op bits[11:8]: 0xA = single
                // (sz=0), 0xB = double (sz=1).  The double form loads/stores
                // BOTH words of the D-register pair; reached after the escape
                // chain closed - esc20 aborted on 0xED9F0B0B (vldr d0,[pc,#44]).
                let double = (w >> 8) & 15 == 11;
                let load = w & (1 << 20) != 0;
                let pre = w & (1 << 24) != 0;
                let up = w & (1 << 23) != 0;
                let writeback = w & (1 << 21) != 0;
                let rn = ((w >> 16) & 15) as usize;
                let imm = (w & 255) * 4;
                let base_expr = if rn == 15 {
                    format!("0x{:08X}u", addr + 8)
                } else {
                    format!("r[{rn}]")
                };
                let sign = if up { "+" } else { "-" };
                let adjusted = format!("({base_expr} {sign} {imm}u)");
                let ea = if pre {
                    adjusted.clone()
                } else {
                    base_expr.clone()
                };
                let mut rejected = false;
                let sd = if double {
                    // D-register field: Vd = {bit22, bits[15:12]}; only the
                    // 16-double bank behind fpr[32] exists (d16+ would alias
                    // fpscr) - loud reject, never silent execution (R2-01).
                    let d_idx = (((w >> 12) & 15) | (((w >> 22) & 1) << 4)) as usize;
                    if d_idx >= 16 {
                        rejected = true;
                        0
                    } else {
                        d_idx * 2
                    }
                } else {
                    (((w >> 12) & 15) * 2 + ((w >> 22) & 1)) as usize
                };
                if rejected {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else if load {
                    if double {
                        body.push_str(&format!(
                            "fpr[{sd}] = mem32({ea}); fpr[{sd2}] = mem32(({ea}) + 4u);",
                            sd = sd,
                            sd2 = sd + 1,
                            ea = ea
                        ));
                    } else {
                        body.push_str(&format!("fpr[{sd}] = mem32({ea});"));
                    }
                } else if double {
                    body.push_str(&format!(
                        "put32({ea}, fpr[{sd}]); put32(({ea}) + 4u, fpr[{sd2}]);",
                        sd = sd,
                        sd2 = sd + 1,
                        ea = ea
                    ));
                } else {
                    body.push_str(&format!("put32({ea}, fpr[{sd}]);"));
                }
                if !rejected && rn != 15 && (!pre || writeback) {
                    body.push_str(&format!(" r[{rn}] = {adjusted};"));
                }
            } else if w & 0x0FFF_0F00 == 0x0E07_0F00 {
                body.push_str("__sync_synchronize();");
            } else if w & 0xFD70_F000 == 0xF550_F000 {
                body.push_str("/* PLD hint */");
            } else if w == 0xE320_F000 {
                // This ARMv6 NOP hint shares the data-processing encoding
                // space with TEQ. It must preserve flags used by later branches.
                body.push_str("/* ARMv6 NOP hint */");
            } else if w == 0xF57F_F01F {
                body.push_str("exclusive_addr = 0xFFFFFFFFu;");
            } else if w & 0x0FFF_0FFF == 0x0E1D_0F70 {
                // MRC p15,0,Rd,c13,c0,3 reads TPIDRURW (the 3DS thread-local
                // storage base). Keep it as explicit guest runtime state.
                let rd = ((w >> 12) & 15) as usize;
                body.push_str(&format!("r[{rd}] = active_tls_base;"));
            } else if cond == 15 && w & 0x0F00_0000 == 0x0F00_0000 {
                body.push_str(&format!("guest_udf(0x{:06X}u);", w & 0x00FF_FFFF));
            } else if w & 0x0F00_0000 == 0x0F00_0000 {
                let svc = w & 0x00FF_FFFF;
                match svc {
                    0x03 => body.push_str("svc_exit_process();"),
                    0x08 => body.push_str("r[0] = svc_create_thread(r);"),
                    0x09 => body.push_str("svc_exit_thread(); running = 0;"),
                    0x0A => body.push_str("svc_sleep((uint64_t)r[0] | ((uint64_t)r[1] << 32)); if (thread_yield_requested) { thread_yield_requested=0; thread_stopped_by_yield=1; running=0; }"),
                    0x13 => body.push_str("r[0] = svc_create_mutex(r);"),
                    0x14 => body.push_str("r[0] = svc_release_mutex(r[0]);"),
                    0x17 => body.push_str("r[0] = svc_create_event(r);"),
                    0x18 => body.push_str("r[0] = svc_signal_event(r[0]);"),
                    0x19 => body.push_str("r[0] = svc_clear_event(r[0]);"),
                    0x20 => body.push_str("r[0] = svc_unmap_memory_block(r[0], r[1]);"),
                    0x1F => body.push_str("r[0] = svc_map_memory_block(r[0], r[1]);"),
                    0x1E => body.push_str("r[1] = next_memory_block_handle++; r[0] = 0;"),
                    0x21 => body.push_str("r[0] = svc_create_address_arbiter(r);"),
                    0x24 => body.push_str("r[0] = svc_wait_synchronization(r[0], (uint64_t)r[2] | ((uint64_t)r[3] << 32)); if (thread_yield_requested) { thread_yield_requested=0; thread_stopped_by_yield=1; running=0; }"),
                    0x25 => body.push_str("r[0] = svc_wait_synchronization_n(r); if (thread_yield_requested) { thread_yield_requested=0; thread_stopped_by_yield=1; running=0; }"),
                    // DuplicateHandle returns the copied handle in r1 while
                    // the wrapper stores it through its saved output pointer.
                    0x27 => body.push_str("r[1] = svc_duplicate_handle(r[1]); r[0] = 0;"),
                    0x28 => body.push_str("svc_get_system_tick(r);"),
                    0x2B => body.push_str("r[0] = svc_get_process_info(r);"),
                    0x2D => body.push_str("r[0] = svc_connect_to_port(r);"),
                    0x32 => body.push_str("r[0] = svc_send_sync(r[0]); if (thread_yield_requested) { thread_yield_requested=0; thread_stopped_by_yield=1; running=0; }"),
                    0x35 => body.push_str("r[0] = svc_get_process_id(r[0], r[1]);"),
                    0x01 => body.push_str("r[0] = svc_control_memory(r);"),
                    0x37 => body.push_str("r[0] = svc_get_thread_id(r);"),
                    0x38 => body.push_str("r[0] = svc_get_resource_limit(r);"),
                    0x39 => body.push_str("r[0] = svc_get_resource_values(r, 1);"),
                    0x3A => body.push_str("r[0] = svc_get_resource_values(r, 0);"),
                    0x22 => {
                        body.push_str("r[0] = svc_arbitrate(r[0], r[1], r[2], r[3], r[4], r[5]); if (thread_yield_requested) { thread_yield_requested=0; thread_stopped_by_yield=1; running=0; }")
                    }
                    0x23 => body.push_str("r[0] = svc_close_handle(r[0]);"),
                    0x3C => body.push_str("svc_break(r);"),
                    _ => {
                        body.push_str(&format!("guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"));
                        changes_pc = true;
                        ended = unconditional;
                    }
                }
            } else if w & 0x0FFF_0FF0 == 0x016F_0F10 {
                // ARM CLZ is a miscellaneous instruction, not data processing.
                // It must leave NZCV unchanged; CLZ(0) is defined as 32.
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                body.push_str(&format!(
                    "r[{rd}] = r[{rm}] ? (uint32_t)__builtin_clz(r[{rm}]) : 32u;"
                ));
            } else if w & 0x0FF0_0030 == 0x0680_0010 {
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                let amount = (w >> 7) & 31;
                if w & 0x40 == 0 {
                    body.push_str(&format!(
                        "r[{rd}] = (r[{rn}] & 0xFFFFu) | ((r[{rm}] << {amount}) & 0xFFFF0000u);"
                    ));
                } else {
                    let shift = if amount == 0 { 32 } else { amount };
                    body.push_str(&format!("r[{rd}] = (r[{rn}] & 0xFFFF0000u) | ((uint32_t)((int32_t)r[{rm}] >> {shift}) & 0xFFFFu);"));
                }
            } else if w & 0x0C00_0000 == 0x0400_0000
                && w & 0x0300_0000 == 0x0200_0000
                && w & 0x0000_0F10 == 0x0000_0F10
                && matches!((w >> 20) & 15, 1 | 2 | 3 | 5 | 6 | 7 | 8)
            {
                // ARMv6 parallel (SIMD) arithmetic register family:
                // SADD8/QADD8/SHADD8/UADD8/UQADD8/UHADD8, the 16-bit and ASX/
                // SAX forms, and SEL.  They share the single data transfer
                // encoding space (bits[27:26]=01, bit25=1, Rs=15, bit4=1), so
                // they must be matched before the generic LDR/STR path.
                // DQ8's movie-decoder blend loop at 0x00195C20 is written
                // entirely in this family; execution reached 0x00195D7C
                // (UHADD8 r12, r2, r3) and aborted as an unknown opcode.
                let op1 = ((w >> 20) & 15) as usize;
                let low = ((w >> 4) & 15) as usize; // (op2 << 1) | 1
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                if rd == 15 || rn == 15 || rm == 15 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else if op1 == 8 && low == 11 {
                    // SEL: per byte lane take Rn when GE[i] is set.
                    body.push_str(&format!(
                        "r[{rd}] = par_sel(r[{rn}], r[{rm}], ge);"
                    ));
                } else if op1 != 8 && matches!(low, 1 | 3 | 5 | 7 | 9 | 15) {
                    let mode = match op1 {
                        1 | 5 => 0u32, // plain SADD/SSUB/UADD/USUB: set GE
                        2 => 1u32,    // saturating signed (Q)
                        3 => 3u32,    // halving signed (SH)
                        6 => 2u32,    // saturating unsigned (UQ)
                        _ => 4u32,    // halving unsigned (UH)
                    };
                    let (bits, kind) = match low {
                        1 => (16u32, 0u32), // <op>16
                        3 => (16, 2),       // ASX
                        5 => (16, 3),       // SAX
                        7 => (16, 1),       // <op>16 subtract
                        9 => (8, 0),        // <op>8
                        _ => (8, 1),        // <op>8 subtract
                    };
                    body.push_str(&format!(
                        "r[{rd}] = par_op(r[{rn}], r[{rm}], {bits}u, {kind}u, {mode}u, &ge);"
                    ));
                } else {
                    // The 8-bit cross forms (ADDSUBX/SUBADDX, op2=0b110) and
                    // SEL with a non-SEL op1 are unused by this title.
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                }
            } else if w & 0x0FFF_FFF0 == 0x012F_FF30 {
                let rm = (w & 15) as usize;
                body.push_str(&format!(
                    "r[14] = 0x{:08X}u; pc = branch_indirect(r[{rm}]);",
                    addr + 4
                ));
                changes_pc = true;
            } else if w & 0x0FFF_FFF0 == 0x012F_FF10 {
                let rm = (w & 15) as usize;
                if rm != 14 {
                    body.push_str(&format!("pc = branch_indirect(r[{rm}]);"));
                    changes_pc = true;
                    ended = unconditional;
                } else {
                    body.push_str("if (r[14] == 0xFFFFFFFFu) running = 0; else pc = r[14];");
                    changes_pc = true;
                    ended = unconditional;
                }
            } else if w & 0x0F00_03F0 == 0x0600_0070
                && matches!((w >> 20) & 15, 8 | 10 | 11 | 12 | 14 | 15)
            {
                // ARMv6 extend family: SXTB/SXTH/UXTB/UXTH plus the halfword
                // packing forms SXTB16/UXTB16 and every accumulate variant
                // (SXTAB/SXTAH/UXTAB/UXTAH/SXTAB16/UXTAB16, i.e. Rn != 15).
                // They share the media encoding space with the parallel
                // arithmetic and PKHBT/PKHTB blocks above (bits[27:24]=0110,
                // bits[9:4]=000111) and are matched before the generic LDR/STR
                // path.  DQ8's movie-decoder blend loops at 0x00194DE4 and
                // 0x00195C20 are built from UXTB16 + ORR byte shuffles; the
                // recompiler previously lifted only the four Rn=15 byte/
                // halfword forms, so execution aborted at 0x00194DE4
                // (UXTB16 r1, r3 = 0xE6CF1073).
                let op1 = ((w >> 20) & 15) as usize;
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                let rotate = ((w >> 10) & 3) * 8;
                let signed = op1 == 8 || op1 == 10 || op1 == 11;
                let half16 = op1 == 8 || op1 == 12; // *B16: two bytes -> two halfwords
                let val = if rotate == 0 {
                    format!("r[{rm}]")
                } else {
                    // Parenthesized: this expression is embedded in larger
                    // `&`/`>>` contexts where `|` would otherwise split it.
                    format!("(r[{rm}] >> {rotate} | r[{rm}] << {})", 32 - rotate)
                };
                let ext = if half16 {
                    // UXTB16/SXTB16: bits[15:0] = ext(byte 0), bits[31:16] =
                    // ext(byte 2) of the rotated value; each half is extended
                    // independently (SXTB16 sign-extends into the full 16-bit
                    // lane, so 0x80 becomes 0xFF80).
                    let lane = |shift: u32| {
                        let src = if shift == 0 {
                            val.clone()
                        } else {
                            format!("({val}) >> {shift}")
                        };
                        if signed {
                            format!(
                                "(uint32_t)(uint16_t)(int16_t)(int8_t)({src} & 0xFFu)"
                            )
                        } else {
                            format!("(uint32_t)(uint16_t)({src} & 0xFFu)")
                        }
                    };
                    format!("{} | ({} << 16)", lane(0), lane(16))
                } else if op1 == 10 || op1 == 14 {
                    // SXTB/SXTAB (sign) and UXTB/UXTAB (zero): one byte.
                    if signed {
                        format!("(uint32_t)(int32_t)(int8_t)({val})")
                    } else {
                        format!("(uint32_t)(uint8_t)({val})")
                    }
                } else {
                    // SXTH/SXTAH (sign) and UXTH/UXTAH (zero): one halfword.
                    if signed {
                        format!("(uint32_t)(int32_t)(int16_t)({val})")
                    } else {
                        format!("(uint32_t)(uint16_t)({val})")
                    }
                };
                if half16 {
                    if rn == 15 {
                        body.push_str(&format!("r[{rd}] = {ext};"));
                    } else {
                        // SXTAB16/UXTAB16: add the two halfwords separately
                        // (no carry propagates between the lanes).  The
                        // parens around {ext} matter: without them `|` in the
                        // lane pair binds looser than `>>`/`&`.
                        body.push_str(&format!(
                            "r[{rd}] = (((r[{rn}] & 0xFFFFu) + (({ext}) & 0xFFFFu)) & 0xFFFFu) | (((((r[{rn}] >> 16) & 0xFFFFu) + (({ext}) >> 16)) & 0xFFFFu) << 16);"
                        ));
                    }
                } else if rn == 15 {
                    body.push_str(&format!("r[{rd}] = {ext};"));
                } else {
                    // SXTAB/SXTAH/UXTAB/UXTAH: Rd = Rn + extended Rm.
                    body.push_str(&format!("r[{rd}] = r[{rn}] + {ext};"));
                }
            } else if matches!(w & 0x0FFF_0FF0, 0x06BF_0F30 | 0x06BF_0FB0 | 0x06FF_0FB0) {
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                match w & 0x0FFF_0FF0 {
                    0x06BF_0F30 => {
                        body.push_str(&format!("r[{rd}] = __builtin_bswap32(r[{rm}]);"))
                    }
                    0x06BF_0FB0 => body.push_str(&format!(
                        "r[{rd}] = ((r[{rm}] & 0x00FF00FFu) << 8) | ((r[{rm}] & 0xFF00FF00u) >> 8);"
                    )),
                    _ => body.push_str(&format!(
                        "r[{rd}] = (uint32_t)(int32_t)(int16_t)(((r[{rm}] & 255u) << 8) | ((r[{rm}] >> 8) & 255u));"
                    )),
                }
            } else if w & 0x0FC0_00F0 == 0x0000_0090 {
                let accumulate = w & (1 << 21) != 0;
                let set_flags = w & (1 << 20) != 0;
                let rd = ((w >> 16) & 15) as usize;
                let rn = ((w >> 12) & 15) as usize;
                let rs = ((w >> 8) & 15) as usize;
                let rm = (w & 15) as usize;
                if [rd, rn, rs, rm].contains(&15) {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                }
                let accum = if accumulate {
                    format!(" + r[{rn}]")
                } else {
                    String::new()
                };
                body.push_str(&format!("r[{rd}] = r[{rm}]*r[{rs}]{accum};"));
                if set_flags {
                    body.push_str(&format!(" z=(r[{rd}]==0); n=(r[{rd}]>>31);"));
                }
            } else if w & 0x0F80_00F0 == 0x0080_0090 {
                // UMULL/UMLAL/SMULL/SMLAL.
                let signed = w & (1 << 22) != 0;
                let accumulate = w & (1 << 21) != 0;
                let set_flags = w & (1 << 20) != 0;
                let rd_hi = ((w >> 16) & 15) as usize;
                let rd_lo = ((w >> 12) & 15) as usize;
                let rs = ((w >> 8) & 15) as usize;
                let rm = (w & 15) as usize;
                if [rd_hi, rd_lo, rs, rm].contains(&15) || rd_hi == rd_lo {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                }
                let ty = if signed { "int64_t" } else { "uint64_t" };
                let lhs = if signed {
                    format!("(int32_t)r[{rm}]")
                } else {
                    format!("r[{rm}]")
                };
                let rhs = if signed {
                    format!("(int32_t)r[{rs}]")
                } else {
                    format!("r[{rs}]")
                };
                let acc = if accumulate {
                    format!("(((uint64_t)r[{rd_hi}] << 32) | r[{rd_lo}]) + ")
                } else {
                    String::new()
                };
                body.push_str(&format!("{{ uint64_t result = (uint64_t)({acc}(({ty})({lhs}) * ({ty})({rhs}))); r[{rd_lo}] = (uint32_t)result; r[{rd_hi}] = (uint32_t)(result >> 32);"));
                if set_flags {
                    body.push_str(" z = (result == 0); n = (int)(result >> 63);");
                }
                body.push_str(" }");
            } else if w & 0x0FF0_0FFF == 0x01F0_0F9F {
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                body.push_str(&format!(
                    "exclusive_addr = r[{rn}]; exclusive_size = 2; r[{rd}] = mem16(exclusive_addr);"
                ));
            } else if let Some((target, link, _)) = arm_branch_target(w, addr) {
                if link {
                    let blx_thumb = w >> 28 == 0xF;
                    if !blx_thumb
                        && let Some(value) =
                            arm_constant_return(&bytes, base, target, known_zero[1])
                    {
                        body.push_str(&format!(
                            "r[14] = 0x{:08X}u; r[0] = 0x{value:08X}u;",
                            addr + 4
                        ));
                    } else {
                        if target >= base && target as usize + 4 <= base as usize + bytes.len() {
                            enqueue_known_seed(&mut pending, &mut force_a32, target, blx_thumb);
                        }
                        body.push_str(&format!(
                            "r[14] = 0x{:08X}u; pc = 0x{:08X}u;",
                            addr + 4,
                            target | u32::from(blx_thumb)
                        ));
                        changes_pc = true;
                        if unconditional && noreturn_calls.contains(&(target & !1)) {
                            // This call restores a saved guest context and
                            // transfers through its saved LR. The following
                            // bytes in the caller may therefore be literals,
                            // not a returning fallthrough path.
                            ended = true;
                        }
                    }
                } else {
                    if target >= base && target as usize + 4 <= base as usize + bytes.len() {
                        enqueue_known_seed(&mut pending, &mut force_a32, target, false);
                    }
                    body.push_str(&format!("pc = 0x{target:08X};"));
                    changes_pc = true;
                    if unconditional {
                        ended = true;
                    }
                }
            } else if matches!(w & 0x0FE0_0030, 0x06A0_0010 | 0x06E0_0010) {
                // ARMv6 SSAT/USAT. These share the single-transfer major
                // opcode space, so recognize them before ordinary LDR/STR.
                let signed = w & 0x0040_0000 == 0;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                let sat = (w >> 16) & 31;
                let amount = (w >> 7) & 31;
                let kind = if w & (1 << 6) != 0 { 2 } else { 0 };
                if rd == 15 || rm == 15 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else {
                    let shifted_amount = if kind == 2 && amount == 0 { 32 } else { amount };
                    let value = if shifted_amount == 0 {
                        format!("(int32_t)r[{rm}]")
                    } else {
                        format!("(int32_t)arm_shift(r[{rm}], {kind}u, {shifted_amount}u)")
                    };
                    let helper = if signed { "arm_ssat" } else { "arm_usat" };
                    body.push_str(&format!("r[{rd}] = {helper}({value}, {sat}u);"));
                }
            } else if matches!(w & 0x0FF0_0FF0, 0x06A0_0F30 | 0x06E0_0F30) {
                // ARMv6 SSAT16/USAT16 saturate each signed halfword lane.
                let signed = w & 0x0040_0000 == 0;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                let sat = (w >> 16) & 15;
                if rd == 15 || rm == 15 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else {
                    let helper = if signed { "arm_ssat16" } else { "arm_usat16" };
                    body.push_str(&format!("r[{rd}] = {helper}(r[{rm}], {sat}u);"));
                }
            } else if w & 0x0E00_0000 == 0x0800_0000 {
                let load = w & (1 << 20) != 0;
                let pre = w & (1 << 24) != 0;
                let up = w & (1 << 23) != 0;
                let writeback = w & (1 << 21) != 0;
                let rn = ((w >> 16) & 15) as usize;
                let list = w & 0xFFFF;
                let regs: Vec<usize> = (0..16).filter(|r| list & (1 << r) != 0).collect();
                if regs.is_empty() || rn == 15 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                }
                if load && writeback && list & (1 << rn) != 0 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                }
                let n = regs.len();
                let start_expr = if up {
                    if pre {
                        format!("r[{rn}] + 4")
                    } else {
                        format!("r[{rn}]")
                    }
                } else if pre {
                    format!("r[{rn}] - {}", n * 4)
                } else {
                    format!("r[{rn}] - {}", (n - 1) * 4)
                };
                body.push_str(&format!("{{ uint32_t a={start_expr};"));
                for (i, reg) in regs.iter().enumerate() {
                    if load {
                        if *reg == 15 {
                            body.push_str(&format!(
                                " {{ uint32_t loaded_pc=mem32(a + {}); if (loaded_pc == 0xFFFFFFFFu) running=0; else pc=loaded_pc; }}",
                                i * 4
                            ));
                            changes_pc = true;
                            // A conditional LDM-to-PC can return on its true
                            // path and continue at the next instruction when
                            // its condition fails. Keep translating that
                            // fallthrough path for the outer conditional case.
                            ended = unconditional;
                        } else {
                            body.push_str(&format!(" r[{reg}] = mem32(a + {});", i * 4));
                        }
                    } else if *reg == 15 {
                        body.push_str(&format!(" put32(a + {}, 0x{:08X});", i * 4, addr + 12));
                    } else {
                        body.push_str(&format!(" put32(a + {}, r[{reg}]);", i * 4));
                    }
                }
                if writeback {
                    if up {
                        body.push_str(&format!(" r[{rn}] += {}u;", n * 4));
                    } else {
                        body.push_str(&format!(" r[{rn}] -= {}u;", n * 4));
                    }
                }
                body.push_str(" }");
            } else if w & 0x0E00_0090 == 0x0000_0090 && (w >> 5) & 3 != 0 {
                // ARM extra load/store encodings: halfword and signed-byte
                // loads, plus the paired-word STRD form used by this title.
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let op = (w >> 5) & 3;
                let load = w & (1 << 20) != 0;
                let offset = if w & (1 << 22) != 0 {
                    format!("{}u", ((w >> 4) & 0xF0) | (w & 15))
                } else {
                    let rm = (w & 15) as usize;
                    format!("r[{rm}]")
                };
                if rn == 15 {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                }
                let sign = if w & (1 << 23) != 0 { "+" } else { "-" };
                let base_expr = format!("r[{rn}]");
                let adjusted = format!("({base_expr} {sign} ({offset}))");
                let address = if w & (1 << 24) != 0 {
                    adjusted.clone()
                } else {
                    base_expr.clone()
                };
                match (load, op) {
                    (false, 1) => body.push_str(&format!("put16({address}, r[{rd}]);")),
                    (false, 2) => {
                        if rd >= 14 || rd & 1 != 0 {
                            body.push_str(&format!(
                                "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                            ));
                            changes_pc = true;
                            ended = unconditional;
                        }
                        body.push_str(&format!(
                            "uint32_t _ldrd_addr = {address}; r[{rd}] = mem32(_ldrd_addr); r[{}] = mem32(_ldrd_addr + 4u);",
                            rd + 1
                        ));
                    }
                    (false, 3) => {
                        if rd >= 14 || rd & 1 != 0 {
                            body.push_str(&format!(
                                "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                            ));
                            changes_pc = true;
                            ended = unconditional;
                        }
                        body.push_str(&format!(
                            "put32({address}, r[{rd}]); put32({address} + 4u, r[{}]);",
                            rd + 1
                        ));
                    }
                    (true, 1) => body.push_str(&format!("r[{rd}] = mem16({address});")),
                    (true, 2) => body.push_str(&format!(
                        "r[{rd}] = (uint32_t)(int32_t)(int8_t)mem8({address});"
                    )),
                    (true, 3) => body.push_str(&format!(
                        "r[{rd}] = (uint32_t)(int32_t)(int16_t)mem16({address});"
                    )),
                    _ => {
                        return Err(format!(
                            "unsupported extra load/store variant at 0x{addr:08X}"
                        ));
                    }
                }
                if w & (1 << 24) == 0 || w & (1 << 21) != 0 {
                    body.push_str(&format!(" r[{rn}] = {adjusted};"));
                }
            } else if w & 0x0FF0_0FF0 == 0x01A0_0F90 {
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                if rm >= 14 || rm & 1 != 0 || rd == rn || rm == rn {
                    return Err(format!("invalid ARM STREXD register pair at 0x{addr:08X}"));
                }
                body.push_str(&format!("if (exclusive_addr == r[{rn}] && exclusive_size == 8u) {{ put32(r[{rn}], r[{rm}]); put32(r[{rn}] + 4u, r[{}]); r[{rd}] = 0; }} else r[{rd}] = 1; exclusive_addr = 0xFFFFFFFFu;", rm + 1));
            } else if w & 0x0FF0_0FFF == 0x01B0_0F9F {
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                if rd >= 14 || rd & 1 != 0 || rd == rn || rd + 1 == rn {
                    return Err(format!("invalid ARM LDREXD register pair at 0x{addr:08X}"));
                }
                body.push_str(&format!("exclusive_addr = r[{rn}]; exclusive_size = 8u; r[{rd}] = mem32(exclusive_addr); r[{}] = mem32(exclusive_addr + 4u);", rd + 1));
            } else if w & 0x0FF0_0FF0 == 0x01E0_0F90 {
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                body.push_str(&format!("if (exclusive_addr == r[{rn}]) {{ put16(r[{rn}], r[{rm}]); r[{rd}] = 0; }} else r[{rd}] = 1; exclusive_addr = 0xFFFFFFFFu;"));
            } else if w & 0x0FF0_0FF0 == 0x0180_0F90 {
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let rm = (w & 15) as usize;
                body.push_str(&format!("if (exclusive_addr == r[{rn}]) {{ if (exclusive_size == 2u) put16(r[{rn}], r[{rm}]); else put32(r[{rn}], r[{rm}]); r[{rd}] = 0; }} else r[{rd}] = 1; exclusive_addr = 0xFFFFFFFFu;"));
            } else if w & 0x0FF0_0FFF == 0x0190_0F9F {
                // ARM LDREX word load; the reservation is used by STREX.
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                body.push_str(&format!(
                    "exclusive_addr = r[{rn}]; exclusive_size = 4; r[{rd}] = mem32(exclusive_addr);"
                ));
            } else if (w & 0x0FF0_0090 == 0x0700_0010)
                || (w & 0x0FF0_0090 == 0x0740_0010)
                || (w & 0x0FF0_0010 == 0x0750_0010)
                || (w & 0x0F90_0090 == 0x0100_0080)
                || (w & 0x0F90_00F0 == 0x0100_0050)
            {
                // ARMv5TE/ARMv6 media multiply + saturating family: SMLAD/SMUAD/
                // SMLSD/SMUSD, SMLALD/SMLSLD, SMMUL/SMMLA/SMMLS, the halfword
                // multiplies SMLAxy/SMULxy/SMLAWy/SMULWy/SMLALxy, and
                // QADD/QSUB/QDADD/QDSUB.  Every one of these shares the
                // bits[27:24]=0001/0111 corner with the generic load/store and
                // data-processing spaces, so they MUST be matched before both:
                //   * SMLAD (0xE701CB17) at pc=0x00189B08 fell into the generic
                //     LDR/STR arm, which flags the register-offset bit4=1 shape
                //     as unsupported -> exit(15) in the round-18 REAL operator
                //     run (esc35) inside the fixed-point DSP routine at
                //     0x00189ABC right after the in-game START press;
                //   * SMLAxy/QADD (bits[27:26]=00) fell through to the
                //     data-processing arm and were SILENTLY misdecoded as
                //     logical/arith ops one instruction later.
                // References (both local): dyncom interpreter semantics
                // (arm_dyncom_interpreter.cpp SMLAD_INST / SMLA_INST /
                // SMULW_INST / SMLALXY_INST / SMMUL_INST / QADD_INST) and
                // azahar fastinterp decode masks (fastinterp_decode.cpp
                // "Halfword multiplies" / "QADD/QSUB/QDADD/QDSUB" blocks,
                // arm_misc.inc handlers), cross-checked against llvm-mc
                // round-trip encodings of every variant.  The Q flag (CPSR
                // bit 27) is not modeled by this runtime, matching the
                // existing SSAT/USAT arms, which also skip it.
                let half = |reg: usize, top: bool| -> String {
                    if top {
                        format!("(int32_t)(int16_t)((r[{reg}] >> 16) & 0xFFFFu)")
                    } else {
                        format!("(int32_t)(int16_t)(r[{reg}] & 0xFFFFu)")
                    }
                };
                let (media, rejected) = if w & 0x0FF0_0090 == 0x0700_0010 {
                    // SMLAD/SMUAD/SMLSD/SMUSD: bits27-20=0x70, bit6 selects
                    // sub (p1-p2) vs add (p1+p2), bit5 swaps Rm halves (X),
                    // bit4=1.  Rd=r16-19 Ra=r12-15 Rm=r8-11 Rn=r0-3;
                    // Ra==15 means no accumulate (the SMUAD/SMUSD forms).
                    let rd = ((w >> 16) & 15) as usize;
                    let ra = ((w >> 12) & 15) as usize;
                    let rm = ((w >> 8) & 15) as usize;
                    let rn = (w & 15) as usize;
                    if rd == 15 || rn == 15 || rm == 15 {
                        (String::new(), true)
                    } else {
                        let rmv = if (w >> 5) & 1 == 1 {
                            format!("(((r[{rm}] & 0xFFFFu) << 16) | (r[{rm}] >> 16))")
                        } else {
                            format!("r[{rm}]")
                        };
                        let sum = if (w >> 6) & 1 == 1 {
                            "(_p1 - _p2)"
                        } else {
                            "(_p1 + _p2)"
                        };
                        let acc = if ra != 15 {
                            format!(" _s += r[{ra}];")
                        } else {
                            String::new()
                        };
                        (format!("{{ uint32_t _rmv = {rmv}; uint32_t _p1 = (uint32_t)((int32_t)(int16_t)(r[{rn}] & 0xFFFFu) * (int32_t)(int16_t)(_rmv & 0xFFFFu)); uint32_t _p2 = (uint32_t)((int32_t)(int16_t)(r[{rn}] >> 16) * (int32_t)(int16_t)(_rmv >> 16)); uint32_t _s = {sum};{acc} r[{rd}] = _s; }}"), false)
                    }
                } else if w & 0x0FF0_0090 == 0x0740_0010 {
                    // SMLALD/SMLSLD: bits27-20=0x74, bit6 sub vs add, bit5
                    // swaps Rm halves (X).  RdHi=r16-19 RdLo=r12-15.
                    let rdhi = ((w >> 16) & 15) as usize;
                    let rdlo = ((w >> 12) & 15) as usize;
                    let rm = ((w >> 8) & 15) as usize;
                    let rn = (w & 15) as usize;
                    if rdhi == 15 || rdlo == 15 || rdhi == rdlo || rn == 15 || rm == 15 {
                        (String::new(), true)
                    } else {
                        let rmv = if (w >> 5) & 1 == 1 {
                            format!("(((r[{rm}] & 0xFFFFu) << 16) | (r[{rm}] >> 16))")
                        } else {
                            format!("r[{rm}]")
                        };
                        let sum = if (w >> 6) & 1 == 1 {
                            "(int64_t)(int32_t)_p1 - (int64_t)(int32_t)_p2"
                        } else {
                            "(int64_t)(int32_t)_p1 + (int64_t)(int32_t)_p2"
                        };
                        (format!("{{ uint32_t _rmv = {rmv}; int32_t _p1 = (int32_t)(int16_t)(r[{rn}] & 0xFFFFu) * (int32_t)(int16_t)(_rmv & 0xFFFFu); int32_t _p2 = (int32_t)(int16_t)(r[{rn}] >> 16) * (int32_t)(int16_t)(_rmv >> 16); int64_t _s = {sum}; uint64_t _pair = ((uint64_t)r[{rdhi}] << 32) | (uint64_t)r[{rdlo}]; _pair += (uint64_t)_s; r[{rdlo}] = (uint32_t)_pair; r[{rdhi}] = (uint32_t)(_pair >> 32); }}"), false)
                    }
                } else if w & 0x0FF0_0010 == 0x0750_0010 {
                    // SMMUL/SMMLA/SMMLS (+R round variants): bits27-20=0x75.
                    // bits[7:6]=00 -> product [+ Ra<<32], 11 -> Ra<<32 minus
                    // product, anything else is not an allocated encoding;
                    // bit5 adds the rounding term 0x80000000 before taking
                    // the high word; Ra==15 = no accumulate (SMMUL/SMMULR).
                    let rd = ((w >> 16) & 15) as usize;
                    let ra = ((w >> 12) & 15) as usize;
                    let rm = ((w >> 8) & 15) as usize;
                    let rn = (w & 15) as usize;
                    let op_hi = (w >> 6) & 3;
                    if rd == 15 || rn == 15 || rm == 15 || (op_hi != 0 && op_hi != 3) {
                        (String::new(), true)
                    } else {
                        let acc = if ra != 15 {
                            if op_hi == 3 {
                                format!("uint64_t _acc = (uint64_t)r[{ra}] << 32; _res = _acc - _res;")
                            } else {
                                format!("uint64_t _acc = (uint64_t)r[{ra}] << 32; _res = _acc + _res;")
                            }
                        } else {
                            String::new()
                        };
                        let rnd = if (w >> 5) & 1 == 1 {
                            " _res += 0x80000000ull;"
                        } else {
                            ""
                        };
                        (format!("{{ int64_t _prod = (int64_t)(int32_t)r[{rn}] * (int64_t)(int32_t)r[{rm}]; uint64_t _res = (uint64_t)_prod; {acc}{rnd} r[{rd}] = (uint32_t)(_res >> 32); }}"), false)
                    }
                } else if w & 0x0F90_0090 == 0x0100_0080 {
                    // Halfword multiplies (fastinterp mask 0x01000080):
                    // bits[22:21] op: 0=SMLAxy 1=SMLAWy(bit5=0)/SMULWy(bit5=1)
                    // 2=SMLALxy 3=SMULxy; bit5=x half of r0-3, bit6=y half of
                    // r8-11.  Rd/r16-19; SMLAxy Ra=r12-15; SMLALxy r12-15=RdLo.
                    let op = (w >> 21) & 3;
                    let x = (w >> 5) & 1 == 1;
                    let y = (w >> 6) & 1 == 1;
                    let rd = ((w >> 16) & 15) as usize;
                    let rl = ((w >> 12) & 15) as usize;
                    let rs = ((w >> 8) & 15) as usize;
                    let r0 = (w & 15) as usize;
                    match op {
                        0 => {
                            // SMLAxy: Rd = Ra + r0[x] * rs[y].
                            if rd == 15 || rl == 15 || rs == 15 || r0 == 15 {
                                (String::new(), true)
                            } else {
                                (format!("r[{rd}] = (uint32_t)({} * {}) + r[{rl}];", half(r0, x), half(rs, y)), false)
                            }
                        }
                        1 if x => {
                            // SMULWy: Rd = bits[47:16] of (s32 r0 * s16 rs[y]).
                            if rd == 15 || rs == 15 || r0 == 15 {
                                (String::new(), true)
                            } else {
                                (format!("r[{rd}] = (uint32_t)((uint64_t)((int64_t)(int32_t)r[{r0}] * (int64_t)({})) >> 16);", half(rs, y)), false)
                            }
                        }
                        1 => {
                            // SMLAWy: same window with Ra<<16 accumulated first.
                            if rd == 15 || rl == 15 || rs == 15 || r0 == 15 {
                                (String::new(), true)
                            } else {
                                (format!("r[{rd}] = (uint32_t)((uint64_t)((int64_t)(int32_t)r[{r0}] * (int64_t)({}) + ((int64_t)(int32_t)r[{rl}]) * 65536LL) >> 16);", half(rs, y)), false)
                            }
                        }
                        2 => {
                            // SMLALxy: 64-bit pair += r0[x] * rs[y].
                            let rdhi = rd;
                            let rdlo = rl;
                            if rdhi == 15 || rdlo == 15 || rdhi == rdlo || rs == 15 || r0 == 15 {
                                (String::new(), true)
                            } else {
                                (format!("{{ int32_t _p = {} * {}; uint64_t _pair = ((uint64_t)r[{rdhi}] << 32) | (uint64_t)r[{rdlo}]; _pair += (uint64_t)(int64_t)_p; r[{rdlo}] = (uint32_t)_pair; r[{rdhi}] = (uint32_t)(_pair >> 32); }}", half(r0, x), half(rs, y)), false)
                            }
                        }
                        _ => {
                            // SMULxy: Rd = r0[x] * rs[y].
                            if rd == 15 || rs == 15 || r0 == 15 {
                                (String::new(), true)
                            } else {
                                (format!("r[{rd}] = (uint32_t)({} * {});", half(r0, x), half(rs, y)), false)
                            }
                        }
                    }
                } else {
                    // QADD/QSUB/QDADD/QDSUB (fastinterp arm_misc.inc order):
                    // Rd=r12-15, primary rm=r0-3, rn=r16-19 (doubled operand
                    // for QDADD/QDSUB).  Signed saturating to int32.
                    let rd = ((w >> 12) & 15) as usize;
                    let rq = ((w >> 16) & 15) as usize;
                    let rmq = (w & 15) as usize;
                    let op = (w >> 21) & 3;
                    if rd == 15 || rq == 15 || rmq == 15 {
                        (String::new(), true)
                    } else {
                        let sat = "_s > 2147483647LL ? 0x7FFFFFFFu : (_s < -2147483648LL ? 0x80000000u : (uint32_t)_s)";
                        match op {
                            0 => (format!("{{ int64_t _s = (int64_t)(int32_t)r[{rmq}] + (int64_t)(int32_t)r[{rq}]; r[{rd}] = {sat}; }}"), false),
                            1 => (format!("{{ int64_t _s = (int64_t)(int32_t)r[{rmq}] - (int64_t)(int32_t)r[{rq}]; r[{rd}] = {sat}; }}"), false),
                            2 => (format!("{{ int64_t _d = (int64_t)(int32_t)r[{rq}] * 2; _d = _d > 2147483647LL ? 2147483647LL : (_d < -2147483648LL ? -2147483648LL : _d); int64_t _s = (int64_t)(int32_t)r[{rmq}] + _d; r[{rd}] = {sat}; }}"), false),
                            _ => (format!("{{ int64_t _d = (int64_t)(int32_t)r[{rq}] * 2; _d = _d > 2147483647LL ? 2147483647LL : (_d < -2147483648LL ? -2147483648LL : _d); int64_t _s = (int64_t)(int32_t)r[{rmq}] - _d; r[{rd}] = {sat}; }}"), false),
                        }
                    }
                };
                if rejected {
                    body.push_str(&format!(
                        "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                    ));
                    changes_pc = true;
                    ended = unconditional;
                } else {
                    body.push_str(&media);
                }
            } else if w & 0x0C00_0000 == 0x0400_0000 {
                let load = w & (1 << 20) != 0;
                let byte = w & (1 << 22) != 0;
                let pre = w & (1 << 24) != 0;
                let up = w & (1 << 23) != 0;
                let wb = w & (1 << 21) != 0;
                let rn = ((w >> 16) & 15) as usize;
                let rd = ((w >> 12) & 15) as usize;
                let delta = if w & (1 << 25) != 0 {
                    if w & (1 << 4) != 0 {
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                        ));
                        changes_pc = true;
                        ended = unconditional;
                        String::from("0u")
                    } else {
                        let rm = (w & 15) as usize;
                        let amount = (w >> 7) & 31;
                        let kind = (w >> 5) & 3;
                        format!("arm_shift_imm(r[{rm}], {kind}, {amount}, c)")
                    }
                } else {
                    format!("0x{:X}u", w & 0xFFF)
                };
                let sign = if up { "+" } else { "-" };
                let base_expr = if rn == 15 {
                    format!("0x{:08X}u", addr + 8)
                } else {
                    format!("r[{rn}]")
                };
                // Snapshot every address operand before the transfer. In
                // particular, LDR with writeback may use Rd as Rm in the
                // register-offset expression. If we emit `r[Rd] = mem32(...)`
                // and then compute writeback from `r[Rm]`, the second
                // expression sees the newly loaded value instead of the
                // original index register. DQ8 uses exactly that pattern in
                // its shared-pointer decrement path.
                body.push_str(&format!(
                    "{{ uint32_t _mem_base={base_expr}, _mem_delta={delta}, _mem_adjusted=_mem_base {sign} _mem_delta, _mem_ea={};",
                    if pre { "_mem_adjusted" } else { "_mem_base" }
                ));
                let adjusted = "_mem_adjusted";
                let ea = "_mem_ea";
                if load {
                    if rd == 15 {
                        if byte {
                            return Err(format!("byte load to PC at 0x{addr:08X}"));
                        }
                        if rn == 15 {
                            if let Some(target) = arm_pc_relative_ldr_target(&bytes, base, addr, w)
                            {
                                let target_addr = target & !1;
                                if target_addr >= base
                                    && (target_addr as usize) < base as usize + bytes.len()
                                {
                                    enqueue_known_seed(
                                        &mut pending,
                                        &mut force_a32,
                                        target,
                                        target & 1 != 0,
                                    );
                                }
                            }
                        }
                        for (target, thumb) in
                            arm_pc_relative_jump_table_targets(&bytes, base, addr, w)
                        {
                            enqueue_known_seed(&mut pending, &mut force_a32, target, thumb);
                        }
                        body.push_str(&format!(
                            "pc = mem32({ea}); if (pc == 0xFFFFFFFFu) running = 0;"
                        ));
                        changes_pc = true;
                        ended = unconditional;
                    } else if byte {
                        body.push_str(&format!("r[{rd}] = mem8({ea});"));
                    } else {
                        body.push_str(&format!("r[{rd}] = mem32({ea});"));
                    }
                    if cond == 14 {
                        known_zero[rd] = false;
                    }
                } else {
                    if rn == 15 {
                        // STR with PC base is UNPREDICTABLE in A32, and in
                        // practice linear decode walks into rodata that merely
                        // SHAPES like one - esc31's build aborted on ASCII
                        // "thod" at 0x00571F5C (inside "Method_Init\0
                        // Method_Output\0...").  Aborting the whole generation
                        // here (the old `Err`) makes any root whose decode
                        // reaches data unbuildable; the multiple-transfer arm
                        // above documents the same rule.  Stop THIS block
                        // loudly instead: never lower it as a store (R2-01),
                        // never fail sibling roots.
                        body.push_str(&format!(
                            "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                        ));
                        changes_pc = true;
                        ended = unconditional;
                    } else if byte {
                        body.push_str(&format!("put8({ea}, r[{rd}]);"));
                    } else {
                        body.push_str(&format!("put32({ea}, r[{rd}]);"));
                    }
                }
                if rn != 15 && (!pre || wb) {
                    body.push_str(&format!(" r[{rn}] = {adjusted};"));
                }
                body.push_str(" }");
            } else if w & 0x0C00_0000 == 0 {
                let op = (w >> 21) & 15;
                let set_flags = w & (1 << 20) != 0 || (8..=11).contains(&op);
                let rd = ((w >> 12) & 15) as usize;
                let rn = ((w >> 16) & 15) as usize;
                let rm_reg = (w & 15) as usize;
                let rm_expr = if rm_reg == 15 {
                    format!("0x{:08X}u", addr.wrapping_add(8))
                } else {
                    format!("r[{rm_reg}]")
                };
                let mut shifter_carry = None;
                let operand = if w & (1 << 25) != 0 {
                    let rot = ((w >> 8) & 15) as u32;
                    let x = (w & 255).rotate_right(rot * 2);
                    if rot != 0 {
                        // ARM A8.5.3: a logical operation with S set takes
                        // the shifter carry from bit 31 of the expanded
                        // immediate whenever the immediate rotation is
                        // non-zero; a zero rotation leaves C alone (audit A01).
                        shifter_carry = Some(format!("{}", x >> 31));
                    }
                    format!("0x{x:08X}u")
                } else {
                    let shift = (w >> 7) & 31;
                    if w & (1 << 4) != 0 {
                        if w & (1 << 7) != 0 {
                            body.push_str(&format!(
                                "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                            ));
                            changes_pc = true;
                            ended = unconditional;
                            "0u".to_string()
                        } else {
                            let rs = ((w >> 8) & 15) as usize;
                            let kind = (w >> 5) & 3;
                            shifter_carry = Some(format!(
                                "arm_shift_carry({rm_expr}, {kind}, r[{rs}] & 255u, c)"
                            ));
                            format!("arm_shift({rm_expr}, {kind}, r[{rs}] & 255u)")
                        }
                    } else {
                        match (w >> 5) & 3 {
                            0 => {
                                if shift == 0 {
                                    rm_expr.clone()
                                } else {
                                    format!("({rm_expr} << {shift})")
                                }
                            }
                            1 => {
                                if shift == 0 {
                                    "0u".into()
                                } else {
                                    format!("({rm_expr} >> {shift})")
                                }
                            }
                            2 => {
                                if shift == 0 {
                                    format!("((int32_t){rm_expr} >> 31)")
                                } else {
                                    format!("((uint32_t)((int32_t){rm_expr} >> {shift}))")
                                }
                            }
                            _ => format!("arm_shift_imm({rm_expr}, 3, {shift}, c)"),
                        }
                    }
                };
                if w & (1 << 25) == 0 && w & (1 << 4) == 0 {
                    let kind = (w >> 5) & 3;
                    let shift = (w >> 7) & 31;
                    // Immediate shift encoding: amount 0 means LSR/ASR #32
                    // (and RRX for ROR), not "no shift".  Use the
                    // immediate-form carry helper so a logical-S reads bit 31
                    // of the source instead of silently preserving the old C
                    // (audit A01).
                    shifter_carry =
                        Some(format!("arm_shift_imm_carry({rm_expr}, {kind}, {shift}, c)"));
                }
                let lhs = if rn == 15 {
                    format!("0x{:08X}u", addr + 8)
                } else {
                    format!("r[{rn}]")
                };
                // ARM data-processing instructions read every source before
                // writing Rd.  Keep the values (and incoming carry) alive
                // through flag calculation; otherwise instructions such as
                // ADCS r2,r2,r2 compute their carry from the already-updated
                // destination and subtly corrupt arithmetic helper routines.
                body.push_str(&format!(
                    "uint32_t _dp_a={lhs}, _dp_b={operand}, _dp_cin=(uint32_t)c;"
                ));
                let saved_shifter_carry = shifter_carry.as_ref().map(|carry| {
                    body.push_str(&format!("uint32_t _dp_shcarry={carry};"));
                    "_dp_shcarry".to_string()
                });
                let lhs = "_dp_a".to_string();
                let operand = "_dp_b".to_string();
                shifter_carry = saved_shifter_carry;
                let expr = match op {
                    0 => format!("({lhs} & {operand})"),
                    1 => format!("({lhs} ^ {operand})"),
                    2 | 10 => format!("({lhs} - {operand})"),
                    3 => format!("({operand} - {lhs})"),
                    4 => format!("({lhs} + {operand})"),
                    5 => format!("({lhs} + {operand} + _dp_cin)"),
                    6 => format!("({lhs} - {operand} - (uint32_t)!_dp_cin)"),
                    7 => format!("({operand} - {lhs} - (uint32_t)!_dp_cin)"),
                    8 => format!("({lhs} & {operand})"),
                    9 => format!("({lhs} ^ {operand})"),
                    11 => format!("({lhs} + {operand})"),
                    12 => format!("({lhs} | {operand})"),
                    13 => operand.clone(),
                    14 => format!("({lhs} & ~({operand}))"),
                    15 => format!("~({operand})"),
                    _ => {
                        return Err(format!(
                            "data-processing opcode {op} at 0x{addr:08X} needs carry/overflow support"
                        ));
                    }
                };
                if matches!(op, 8..=11) {
                    if !set_flags {
                        return Err(format!("unexpected non-flag test op at 0x{addr:08X}"));
                    }
                    if op == 10 || op == 11 {
                        if op == 10 {
                            body.push_str(&format!("{{ uint32_t a={lhs}, b={operand}, t=a-b; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}"));
                        } else {
                            body.push_str(&format!("{{ uint32_t a={lhs}, b={operand}, t=a+b; uint64_t wide=(uint64_t)a+b; z=(t==0); n=(t>>31); c=(wide>>32); v=((~(a^b)&(a^t))>>31); }}"));
                        }
                    } else {
                        body.push_str(&format!(
                            "{{ uint32_t t = {expr}; z = (t == 0); n = (t >> 31); }}"
                        ));
                        if let Some(carry) = &shifter_carry {
                            body.push_str(&format!(" c = {carry};"));
                        }
                    }
                } else {
                    if rd == 15 {
                        if set_flags {
                            body.push_str(&format!(
                                "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                            ));
                            changes_pc = true;
                            ended = unconditional;
                        } else {
                            body.push_str(&format!("pc = ({expr}) & ~3u;"));
                            changes_pc = true;
                            ended = unconditional;
                        }
                    } else {
                        if set_flags {
                            body.push_str(&format!(
                                "uint32_t _dp_result={expr}; r[{rd}]=_dp_result;"
                            ));
                        } else {
                            body.push_str(&format!("r[{rd}] = {expr};"));
                        }
                    }
                    if cond == 14 && rd != 15 {
                        known_zero[rd] = match op {
                            13 if w & (1 << 25) != 0 => {
                                ((w & 255).rotate_right(((w >> 8) & 15) * 2)) == 0
                            }
                            13 if w & (1 << 25) == 0
                                && ((w >> 5) & 3) == 0
                                && ((w >> 7) & 31) == 0 =>
                            {
                                known_zero[(w & 15) as usize]
                            }
                            0 => known_zero[rn] && (w & (1 << 25) != 0 && w & 255 == 0),
                            _ => false,
                        };
                    }
                    if set_flags && rd != 15 {
                        match op {
                        2 => body.push_str(&format!("{{ uint32_t a={lhs}, b={operand}, t=_dp_result; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}")),
                        3 => body.push_str(&format!("{{ uint32_t a={operand}, b={lhs}, t=_dp_result; z=(t==0); n=(t>>31); c=(a>=b); v=(((a^b)&(a^t))>>31); }}")),
                        4 => body.push_str(&format!("{{ uint32_t a={lhs}, b={operand}, t=_dp_result; uint64_t wide=(uint64_t)a+b; z=(t==0); n=(t>>31); c=(wide>>32); v=((~(a^b)&(a^t))>>31); }}")),
                        5 => body.push_str(&format!("{{ uint32_t a={lhs}, b={operand}; uint64_t wide=(uint64_t)a+b+_dp_cin; uint32_t t=_dp_result; int64_t sw=(int64_t)(int32_t)a+(int64_t)(int32_t)b+_dp_cin; z=(t==0); n=(t>>31); c=(wide>>32); v=(sw>INT32_MAX || sw<INT32_MIN); }}")),
                        6 => body.push_str(&format!("{{ uint32_t a={lhs}, b={operand}, borrow=(uint32_t)!_dp_cin; uint64_t sub=(uint64_t)b+borrow; uint32_t t=_dp_result; int64_t sw=(int64_t)(int32_t)a-(int64_t)(int32_t)b-borrow; z=(t==0); n=(t>>31); c=((uint64_t)a>=sub); v=(sw>INT32_MAX || sw<INT32_MIN); }}")),
                        7 => body.push_str(&format!("{{ uint32_t a={operand}, b={lhs}, borrow=(uint32_t)!_dp_cin; uint64_t sub=(uint64_t)b+borrow; uint32_t t=_dp_result; int64_t sw=(int64_t)(int32_t)a-(int64_t)(int32_t)b-borrow; z=(t==0); n=(t>>31); c=((uint64_t)a>=sub); v=(sw>INT32_MAX || sw<INT32_MIN); }}")),
                        _ => body.push_str(&format!(" z = (_dp_result == 0); n = (_dp_result >> 31);")),
                    }
                        if matches!(op, 0 | 1 | 12..=15) {
                            if let Some(carry) = &shifter_carry {
                                body.push_str(&format!(" c = {carry};"));
                            }
                        }
                    }
                }
            } else {
                body.push_str(&format!(
                    "guest_unsupported(0x{addr:08X}u, 0x{w:08X}u, 0u);"
                ));
                changes_pc = true;
                ended = unconditional;
            }
            let next = addr.wrapping_add(4);
            if !changes_pc {
                body.push_str(&format!(" pc = 0x{next:08X};"));
            }
            let wrap_condition = if cond == 15 {
                "1"
            } else {
                arm_condition_c(cond)
            };
            if emitted
                .insert(
                    addr,
                    condition_wrap(
                        wrap_condition,
                        &body,
                        &format!("pc = 0x{next:08X};"),
                    ),
                )
                .is_some()
            {
                break;
            }
            total_insns += 1;
            if ended {
                break;
            }
            addr = next;
        }
        if !ended {
            return Err(format!(
                "function 0x{function_start:08X}: no supported BX LR within 4096 instructions (translated {} instructions before stop)",
                total_insns,
            ));
        }
    }
    // S1b: emit ONE straight-line Block-IR block (env-gated at runtime), only
    // when --s1-block selects it.  Without the flag every S1 string below is
    // empty and the generated C stays byte-identical to the pre-S1b output.
    let s1 = match s1_block {
        None => None,
        Some(addr) => {
            if addr & 3 != 0 {
                return Err(format!("--s1-block 0x{addr:08X} is not word aligned"));
            }
            let Some(off) = addr.checked_sub(base).map(|v| v as usize) else {
                return Err(format!("--s1-block 0x{addr:08X} is below the code base"));
            };
            if off + 4 > bytes.len() {
                return Err(format!("--s1-block 0x{addr:08X} is outside the code image"));
            }
            // Extract a generous window; the emitter rejects a block that is
            // not terminated inside it (truncated window, not a real block).
            let region = block_ir::extract_region(&bytes, base, addr, addr.saturating_add(0x100))
                .map_err(|e| format!("--s1-block 0x{addr:08X}: {e}"))?;
            let block = region
                .blocks
                .first()
                .filter(|b| b.start == addr)
                .ok_or_else(|| {
                    format!("--s1-block 0x{addr:08X}: no basic block starts at the entry")
                })?;
            Some(
                block_emit::emit_block(block, &emitted)
                    .map_err(|e| format!("--s1-block 0x{addr:08X}: {e}"))?,
            )
        }
    };
    // S1c: emit a whole region's basic blocks as ONE native CFG (direct
    // intra-region edges, exits fall back to the dispatcher), only when
    // --s1-cfg/--s1-cfg-end select it.  Without the flags every S1_CFG string
    // below is empty and the generated C stays byte-identical to the output a
    // build without them produces.
    let s1_cfg = match s1_cfg {
        None => None,
        Some(regions) => {
            let mut function_c = String::new();
            let mut hook_c = String::new();
            let mut gate_c = String::new();
            let mut entries = Vec::new();
            for (entry, end) in regions {
                if entry & 3 != 0 {
                    return Err(format!("--s1-cfg 0x{entry:08X} is not word aligned"));
                }
                if end <= entry {
                    return Err(format!(
                        "--s1-cfg-end 0x{end:08X} must be above the region entry 0x{entry:08X}"
                    ));
                }
                let Some(off) = entry.checked_sub(base).map(|v| v as usize) else {
                    return Err(format!("--s1-cfg 0x{entry:08X} is below the code base"));
                };
                if off + 4 > bytes.len() || (end - base) as usize > bytes.len() {
                    return Err(format!(
                        "--s1-cfg region 0x{entry:08X}..0x{end:08X} leaves the code image"
                    ));
                }
                let region = block_ir::extract_region(&bytes, base, entry, end)
                    .map_err(|e| format!("--s1-cfg 0x{entry:08X}: {e}"))?;
                if region.start != entry {
                    return Err(format!(
                        "--s1-cfg 0x{entry:08X}: no basic block starts at the entry"
                    ));
                }
                let part = block_emit::emit_region(&region, &emitted)
                    .map_err(|e| format!("--s1-cfg 0x{entry:08X}..0x{end:08X}: {e}"))?;
                entries.push(part.entry);
                function_c.push_str(&part.function_c);
                hook_c.push_str(&part.hook_c);
                gate_c.push_str(&part.gate_c);
            }
            // S1b prototype: one page prefilter for the whole region-hook
            // chain.  Without it every dispatch iteration pays one
            // `pc == entry` compare per converted region (39 regions would be
            // 39 compares per guest instruction); with it, an iteration pays
            // one table load and leaves the chain unless its 4 KiB page has a
            // converted region.  Emitted only when --s1-cfg is used, so the
            // default generated C is unchanged.
            if !hook_c.is_empty() {
                let mut pages: Vec<u32> = entries.iter().map(|e| e >> 12).collect();
                pages.sort_unstable();
                pages.dedup();
                let mut page_table = String::from(
                    "/* S1 prototype page prefilter for the region hook chain. */\n\
                     static const unsigned char dq8_s1_cfg_page[4096] = {\n",
                );
                for page in &pages {
                    page_table.push_str(&format!("    [0x{page:03X}] = 1,\n"));
                }
                page_table.push_str("};\n");
                function_c.push_str(&page_table);
                // A switch on pc, not a compare chain: with 30+ converted
                // regions a linear `pc == entry` chain cost more than the
                // dispatch it replaced (measured: 0.90 -> 0.77 M instr/s).
                hook_c = format!(
                    "        if (dq8_s1_cfg_page[(pc >> 12) & 0xFFFu]) switch (pc) {{\n{hook_c}        }}\n"
                );
            }
            Some(S1CfgRegions {
                entries,
                function_c,
                hook_c,
                gate_c,
            })
        }
    };
    let mut c = String::from(
        r#"#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>   /* mkdir(2) for DQ8_DUMP_PICA_COUNT_DIR (t_41f38df0) */
#ifdef DQ8_ENABLE_SDL
#include <SDL.h>
#ifdef DQ8_HAVE_X11_PRESENT
#include <SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif
#endif
/* FRAME_PROF_RUNTIME */
/* NATIVE_SPSC_RUNTIME */
/* F4/F5 thread-split gate: DQ8_SPLIT_RENDER=1 moves framebuffer conversion,
   SDL event collection and presentation off the guest loop onto a dedicated
   worker thread fed by src/native_spsc.h queues, and switches vblank
   generation to a wall-clock 60 Hz deadline (see gsp_vblank_service).  The
   default is the frozen inline path, byte-for-byte. */
static int split_render_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("DQ8_SPLIT_RENDER");
        enabled = (e && e[0] == '1' && !e[1]) ? 1 : 0;
    }
    return enabled;
}
static void split_render_flush_sync(void);
static uint32_t split_render_hid_mask(void);
static int split_render_quit_requested(void);
static void split_render_shutdown(void);
static void split_render_start(void);
static uint8_t *ram;
static FILE *romfs_image;
static uint64_t romfs_data_offset;
static uint64_t romfs_data_size;
static uint8_t guest_file_is_romfs[256];
static uint32_t guest_archive_id[256];
static uint8_t guest_dir_open[256];
/* In-memory NCCH archive files (azahar NCCHArchive fallback for missing
   system-archive content).  The name-entry CONFIRM validates the entered
   name against the NG (bad) word list: OpenFileDirectly archive 0x2345678A
   (NCCH), NCCHArchivePath{tid=0x000400DB00010302 (system_data_archive /
   ng_word_list)}, NCCHFilePath{filepath_type=RomFS}.  azahar serves
   BAD_WORD_LIST_DATA via IVFCFileInMemory when the content is not
   installed; we embed the same open-source replacement bytes and serve them
   with equivalent Read/GetSize semantics instead of the synthetic
   zero-fill that left the guest spinning in a read-retry loop. */
static const uint8_t *guest_memfile_data[256];
static uint32_t guest_memfile_size[256];
/* NG_WORD_LIST_DATA */
/* Single application APT mailbox. HOME is headless: a suspend receives
   the same WakeupByPause message as APT LeaveHomeMenu, once only. */
static uint32_t apt_pending_signal = 1u;
static uint32_t apt_notification_event, apt_parameter_event;
#define GUEST_RAM_SIZE 0x40000000u
static uint32_t current_guest_pc;
static uint32_t previous_guest_pc;
static uint32_t main_guest_pc;
static uint32_t *main_guest_regs;
static uint32_t trace_guest_pcs[16];
static unsigned trace_guest_pc_count;
/* GUESTPHASE (t_3de68379): wall-clock time spent inside the guest dispatch
   loop of translated_from() - the "guest interpretation" row of the frame
   budget (reports/FRAMEBUDGET_t_432a85ab.md §3), measured directly instead
   of inferred from RIP samples.  The clock is read at the TOP of every
   dispatch-loop iteration and charged to the PREVIOUS one, so the `continue`
   paths (fast page-list, snprintf shim, movie skip) are accounted too and
   only the final iteration is dropped.  Opt-in (DQ8_GUEST_TIME=1); with the
   knob off it costs one predictable branch per iteration, so both arms of an
   A/B built from the same revision carry the identical instruction. */
static int dq8_guest_time_enabled;
static uint64_t dq8_guest_time_ns;
static uint64_t dq8_guest_time_t0;
/* Thread CPU clock, not the wall clock: the dispatch loop parks inside
   svc_wait_synchronization / host_display_present for far longer than it
   executes, and charging that sleep to "guest interpretation" reports ~100 %
   of the frame instead of the real share. */
static uint64_t dq8_guest_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* A05 boundary fast prefilter (see dq8_pc_needs_boundary): 1 when this 4 KiB
   page may contain a PC that needs the main-loop dispatch boundary.  Statically
   initialised from the same pc list as the exact switch below (generated, so
   the two cannot drift), which keeps the hook-free-page fast path free of any
   runtime setup; runtime DQ8_TRACE_PC pcs are guarded by the trace-list check
   in the predicate itself.  Index wrap for pc >= 16 MiB can only
   over-approximate (extra boundary re-checks), never skip a hook. */
static const unsigned char dq8_pc_boundary_page[4096] = {
/* BOUNDARY_PAGES */
};
static unsigned trace_guest_pc_hits;
static unsigned trace_guest_pc_max = 64u;
/* DQ8_TRACE_WRITE=<va>:<len>[,<va>:<len>...] — up to 4 ranges: the guest may
 * build a command chunk through the LINEAR alias (0x14xxxxxx) while GSP is
 * handed the FCRAM address (0x20xxxxxx) of the same bytes, so both views of one
 * buffer have to be watchable in the same run (t_41f38df0). */
static uint32_t trace_guest_write_addr[4];
static uint32_t trace_guest_write_end[4];
static unsigned trace_write_n;
static unsigned trace_guest_write_hits;
static int trace_write_hit(uint32_t a) {
    for (unsigned i = 0; i < trace_write_n; ++i)
        if (a >= trace_guest_write_addr[i] && a < trace_guest_write_end[i]) return (int)i;
    return -1;
}
/* DQ8_TRACE_HID_READS: log every guest read of the HID shared-memory page
 * (set when the guest maps HID shared memory, see svc_map_memory_block). */
static uint32_t trace_hid_read_base;
static uint32_t trace_hid_read_end;
static int trace_hid_reads_enabled;
static uint64_t trace_hid_t0_ns;
static int trace_hid_runtime_ctx;
struct trace_hid_pc_state { uint32_t pc; unsigned hits; uint32_t last_val; uint64_t last_sec; };
static struct trace_hid_pc_state trace_hid_pcs[64];
static unsigned trace_hid_pc_count;
static void trace_hid_read(uint32_t a, uint32_t v);
static uint32_t trace_format_path_dest;
static uint32_t trace_format_path_base;
static unsigned trace_format_path_char_count;
static int trace_format_path_active;
static uint32_t *active_guest_regs;
static unsigned thread_execution_depth;
static uint64_t dq8_guest_now_ns(void);
static int dq8_det_mode;
static uint64_t dq8_det_steps_total, dq8_det_idle_ns, dq8_det_vblanks, dq8_det_idle_jumps;
static uint64_t perf_main_retired;
/* SAVESTATE (t_d50c9503): F5 save / F9 load.  Declared here because the SDL
   event collector (host_display_collect_events, shared with the split-present
   worker) and translated_from() both need them; the implementation lives in
   src/native_savestate.c, spliced just before translated_from(). */
static void savestate_request(int kind);
static int savestate_service(uint32_t *pc, uint32_t *r,
                             int *n, int *z, int *c, int *v, int *ge);
static int savestate_take_resume(int *n, int *z, int *c, int *v, int *ge);
static int savestate_cold_load(const char *path, uint32_t *pc, uint32_t *r);
static void savestate_set_path(const char *path);
static void guest_dump_threads(void);
/* Host shadow buffers + scan-out state: defined with the host display code
   further below; the periodic progress dump reports their occupancy. */
static uint8_t host_shadow_top[400u * 240u * 4u];
static uint8_t host_shadow_bot[320u * 240u * 4u];
static uint64_t pica_draw_count;
static uint32_t pica_rt_addr, pica_rt_w, pica_rt_h, pica_rt_format;
static uint32_t gsp_fb_addr[2][2];
static uint32_t gsp_fb_shown[2];
static int gsp_fb_valid[2];
static int guest_memory_range(uint32_t, uint64_t, uint32_t *);
static void guest_periodic_dump(int sig) {
    (void)sig;
    if (getenv("DQ8_PROGRESS_SECONDS")) {
        if (dq8_det_mode) {
            fprintf(stderr, "DETDUMP steps=%llu idle_ms=%.3f now_ms=%.3f vblanks=%llu jumps=%llu"
                            " main_retired=%llu depth=%u main_pc=%08X active_pc=%08X\n",
                    (unsigned long long)dq8_det_steps_total,
                    (double)dq8_det_idle_ns / 1e6,
                    (double)dq8_guest_now_ns() / 1e6,
                    (unsigned long long)dq8_det_vblanks,
                    (unsigned long long)dq8_det_idle_jumps,
                    (unsigned long long)perf_main_retired,
                    thread_execution_depth, main_guest_pc, current_guest_pc);
            if (getenv("DQ8_DUMP_STUB")) {
                static unsigned dumped;
                if (!dumped) {
                    FILE *f;
                    dumped = 1;
                    f = fopen("/tmp/dq8-range-stub.bin", "wb");
                    if (f) { fwrite(ram + 0x0012C200u, 1u, 0x300u, f); fclose(f); }
                    f = fopen("/tmp/dq8-range-wave.bin", "wb");
                    if (f) { fwrite(ram + 0x00179800u, 1u, 0x800u, f); fclose(f); }
                }
            }
        }
        fprintf(stderr, "guest snapshot pc=%08X prev=%08X depth=%u main=%08X mainr0-r3=%08X/%08X/%08X/%08X active-r0-r7=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X lr=%08X sp=%08X\n",
                current_guest_pc, previous_guest_pc, thread_execution_depth, main_guest_pc,
                main_guest_regs ? main_guest_regs[0] : 0u,
                main_guest_regs ? main_guest_regs[1] : 0u,
                main_guest_regs ? main_guest_regs[2] : 0u,
                main_guest_regs ? main_guest_regs[3] : 0u,
                active_guest_regs ? active_guest_regs[0] : 0u,
                active_guest_regs ? active_guest_regs[1] : 0u,
                active_guest_regs ? active_guest_regs[2] : 0u,
                active_guest_regs ? active_guest_regs[3] : 0u,
                active_guest_regs ? active_guest_regs[4] : 0u,
                active_guest_regs ? active_guest_regs[5] : 0u,
                active_guest_regs ? active_guest_regs[6] : 0u,
                active_guest_regs ? active_guest_regs[7] : 0u,
                active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u);
        fprintf(stderr, "guest startup-object state=%08X event=%08X queue=%08X extra=%08X flag76=%02X flag77=%02X\n",
                *(uint32_t *)(void *)(ram + 0x005CCCA4u),
                *(uint32_t *)(void *)(ram + 0x005CCC6Cu),
                *(uint32_t *)(void *)(ram + 0x005CCC74u),
                *(uint32_t *)(void *)(ram + 0x005CCCACu),
                ram[0x005CCCB6u], ram[0x005CCCB7u]);
        {
            uint64_t top_nz = 0, bot_nz = 0;
            for (unsigned i = 0; i < 400u * 240u; ++i)
                if (host_shadow_top[i * 4u] | host_shadow_top[i * 4u + 1u] | host_shadow_top[i * 4u + 2u]) ++top_nz;
            for (unsigned i = 0; i < 320u * 240u; ++i)
                if (host_shadow_bot[i * 4u] | host_shadow_bot[i * 4u + 1u] | host_shadow_bot[i * 4u + 2u]) ++bot_nz;
            fprintf(stderr, "presentation state: pica_draws=%llu rt=%08X %ux%u fmt=%u shadow_top_nz=%llu shadow_bot_nz=%llu scanout_top=%08X(shown=%u valid=%d) scanout_bot=%08X\n",
                    (unsigned long long)pica_draw_count, pica_rt_addr, pica_rt_w, pica_rt_h,
                    pica_rt_format, (unsigned long long)top_nz, (unsigned long long)bot_nz,
                    gsp_fb_addr[0][gsp_fb_shown[0]], gsp_fb_shown[0], gsp_fb_valid[0],
                    gsp_fb_addr[1][gsp_fb_shown[1]]);
            /* Where are the pixels? Scan the full PICA RT (512-wide tiles) and
               both GSP scan-out pair members straight from guest RAM. */
            {
                uint64_t rt_nz = 0, so0_nz = 0, so1_nz = 0, src_nz = 0;
                uint32_t mapped;
                if (guest_memory_range(pica_rt_addr, (uint64_t)pica_rt_w * pica_rt_h * 4u, &mapped)) {
                    const uint8_t *q = ram + mapped;
                    uint64_t n = (uint64_t)pica_rt_w * pica_rt_h;
                    for (uint64_t i = 0; i < n; ++i)
                        if (q[i * 4u] | q[i * 4u + 1u] | q[i * 4u + 2u]) ++rt_nz;
                }
                if (guest_memory_range(0x1F070800u, 240u * 400u * 4u, &mapped)) {
                    const uint8_t *q = ram + mapped;
                    for (uint64_t i = 0; i < 240u * 400u; ++i)
                        if (q[i * 4u] | q[i * 4u + 1u] | q[i * 4u + 2u]) ++src_nz;
                }
                if (guest_memory_range(0x1F346500u, 240u * 400u * 3u, &mapped)) {
                    const uint8_t *q = ram + mapped;
                    for (uint64_t i = 0; i < 240u * 400u; ++i)
                        if (q[i * 3u] | q[i * 3u + 1u] | q[i * 3u + 2u]) ++so0_nz;
                }
                if (guest_memory_range(0x1F300000u, 240u * 400u * 3u, &mapped)) {
                    const uint8_t *q = ram + mapped;
                    for (uint64_t i = 0; i < 240u * 400u; ++i)
                        if (q[i * 3u] | q[i * 3u + 1u] | q[i * 3u + 2u]) ++so1_nz;
                }
                fprintf(stderr, "pixel-where: rt_full_nz=%llu transfer_src_1F070800_nz=%llu scanout_1F346500_nz=%llu scanout_1F300000_nz=%llu\n",
                        (unsigned long long)rt_nz, (unsigned long long)src_nz,
                        (unsigned long long)so0_nz, (unsigned long long)so1_nz);
            }
        }
        guest_dump_threads();
        signal(SIGALRM, guest_periodic_dump);
        alarm(5u);
    }
}
typedef struct {
    uint32_t r[16], pc, fpr[32], fpscr, exclusive_addr, exclusive_size;
    uint32_t tls_base;
    uint32_t wait_handle, wait_address, wait_type, wait_value, wait_arbiter;
    int32_t priority;
    uint32_t wait_handles[64], wait_count, wait_all, wait_n_active;
    uint32_t guest_thread_id;
    uint64_t wake_ns;
    uint32_t wake_result; /* r[0] to record when wake_ns expires; 0 = keep r[0] */
    uint64_t retired_steps;   /* PERF-02: instructions this worker retired */
    uint64_t dispatch_calls;  /* PERF-02: scheduler dispatches into this worker */
    uint64_t sleep_calls;     /* PERF-02: SleepThread/wait park calls */
    uint64_t block_since_ns;  /* DQ8_TRACE_MOVIE: when this thread last parked */
    uint64_t blocked_ns;      /* DQ8_TRACE_MOVIE: cumulative parked wall time */
    uint64_t blocked_count;   /* DQ8_TRACE_MOVIE: cumulative park count */
    int n, z, c, v, ge, active, blocked;
} GuestThread;
static GuestThread guest_threads[64];
static unsigned guest_thread_count, guest_thread_cursor;
static uint32_t current_guest_thread_id = 1u;
static void guest_dump_threads(void) {
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        GuestThread *t = &guest_threads[i];
        fprintf(stderr, "guest thread[%u] id=%08X active=%d blocked=%d pc=%08X tls=%08X wait=%08X/%u/%08X handle=%08X\n",
                i, t->guest_thread_id, t->active, t->blocked, t->pc,
                t->tls_base, t->wait_address, t->wait_type, t->wait_value, t->wait_handle);
    }
}
static void translated_from(uint32_t *r, uint32_t start_pc, GuestThread *context);
static void schedule_guest_thread(void);
static int guest_poll_wait_n(const uint32_t *handles, uint32_t count, uint32_t all,
                             uint32_t *index, int consume, uint32_t thread_id);
/* One wait-object state machine, shared by every producer (audit A07):
   readiness -> waiter selection -> atomic acquire/consume -> result recorded
   on the waiter -> runnable.  These four entry points are the only way to
   observe or change waitable state; SignalEvent/ReleaseMutex/HID/Y2R/GSP/DSP
   all go through guest_notify_handle() instead of touching `blocked`. */
static int guest_object_ready(uint32_t handle, uint32_t thread_id);
static int guest_object_acquire(uint32_t handle, uint32_t thread_id);
static int guest_thread_waits_on(const GuestThread *t, uint32_t handle);
static int guest_wait_ready(const GuestThread *t, uint32_t handle);
static unsigned guest_notify_handle(uint32_t handle);
static void guest_wait_finish(GuestThread *t, uint32_t result, uint32_t index, int has_index);
static void guest_wait_release(GuestThread *t);
static int thread_yield_requested;
static int thread_stopped_by_yield;
static int thread_blocked_requested;
static uint32_t thread_wait_handle_requested;
static uint32_t thread_wait_address_requested, thread_wait_type_requested, thread_wait_value_requested;
static uint32_t thread_wait_arbiter_requested;
static uint64_t thread_sleep_requested_ns;
/* r[0] recorded when a parked wait's deadline expires.  Zero keeps the
   provisional result: that is what sleeps and the GSP vblank pacing park
   want (they are not kernel timeouts). */
static uint32_t thread_wake_result_requested;
static uint32_t next_guest_thread_handle = 0x50000u;
static uint32_t next_memory_block_handle = 0x30020u;
static uint32_t active_tls_base = 0x1FF82000u;
static uint32_t exclusive_addr = 0xFFFFFFFFu;
static uint32_t exclusive_size = 4u;
static uint32_t fpr[32];
static uint32_t fpscr;
static uint64_t host_monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}
/* DETERMINISTIC STEPPING (t_115d297c) -- OPT-IN, DEFAULT OFF.
   Renderer A/B needs the guest to reach the same moment at the same
   DQ8_BENCH_STEPS budget.  By default every guest-visible clock (vblank
   chain, svcGetSystemTick, thread sleep/wake deadlines, DSP audio frame
   cadence) is host wall time, so two runs of the same binary diverge
   (measured 22.83% of pixels at 120M steps, t_8510204b).

   With DQ8_DETERMINISTIC=1 the same sources are derived from a virtual
   clock:

     guest_now = base + idle_ns + retired_steps * ns_per_step

   where retired_steps counts EVERY retired guest instruction (main context
   folded every 4096 steps, worker slices folded when their slice ends) and
   idle_ns is virtual time handed out while no guest code can run (a wait
   with no runnable peer jumps the clock to the next deadline instead of
   nanosleep'ing).  The mapping is documented in
   docs/DETERMINISTIC_STEPPING.md: default 400 virtual ns per retired
   instruction, i.e. ~2.5 M instructions per virtual second, which is the
   measured headless-GL throughput of this runtime, so one virtual second
   is about one host second and the vblank chain still runs at 60 Hz of
   guest time.

   With the flag off nothing below is called on the hot paths except the
   `dq8_det_mode` test, and every accessor returns host_monotonic_ns()
   exactly as before. */
static int dq8_det_mode;
static uint64_t dq8_det_ns_per_step = 400ull;
static uint64_t dq8_det_steps_total;   /* retired instructions, main + worker */
static uint64_t dq8_det_idle_ns;       /* virtual ns advanced while idle */
static uint64_t dq8_det_base_ns = 1000000000ull;
static uint64_t dq8_det_main_reported; /* main-context steps already folded */
static uint64_t dq8_det_next_main_mark = 4096ull;
static uint64_t dq8_det_vblanks;
static uint64_t dq8_det_idle_jumps;
static uint64_t dq8_det_audio_frames;
static uint64_t dq8_guest_now_ns(void) {
    if (!dq8_det_mode) return host_monotonic_ns();
    return dq8_det_base_ns + dq8_det_idle_ns + dq8_det_steps_total * dq8_det_ns_per_step;
}
static void dq8_det_fold_worker(uint64_t retired) {
    if (dq8_det_mode) dq8_det_steps_total += retired;
}
static void dq8_det_fold_main(uint64_t local_steps) {
    if (!dq8_det_mode) return;
    if (local_steps > dq8_det_main_reported) {
        dq8_det_steps_total += local_steps - dq8_det_main_reported;
        dq8_det_main_reported = local_steps;
    }
}
static void dq8_det_advance_to(uint64_t target_ns) {
    uint64_t now;
    if (!dq8_det_mode) return;
    now = dq8_guest_now_ns();
    if (target_ns > now) {
        dq8_det_idle_ns += target_ns - now;
        ++dq8_det_idle_jumps;
    }
}
/* ---- MD5 (RFC 1321) over the presented PPM ---------------------------------
   Two runs must produce byte-identical /tmp/dq8-screen.ppm; the digest is
   printed in the log at exit so an A/B can compare without trusting file
   mtimes.  Only reached when DQ8_DETERMINISTIC=1 is set. */
typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; } Dq8Md5;
static const uint32_t dq8_md5_k[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,
    0xa8304613u,0xfd469501u,0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,
    0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,0xf61e2562u,0xc040b340u,
    0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,
    0x676f02d9u,0x8d2a4c8au,0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,
    0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,0x289b7ec6u,0xeaa127fau,
    0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,
    0xffeff47du,0x85845dd1u,0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,
    0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u };
static const uint8_t dq8_md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
#define DQ8_MD5_ROTL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))
static void dq8_md5_block(Dq8Md5 *m, const uint8_t *p) {
    uint32_t w[16], a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3], f, t;
    unsigned i, g;
    for (i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[i*4u] | ((uint32_t)p[i*4u+1u] << 8) |
               ((uint32_t)p[i*4u+2u] << 16) | ((uint32_t)p[i*4u+3u] << 24);
    for (i = 0; i < 64; ++i) {
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5u * i + 1u) & 15u; }
        else if (i < 48) { f = b ^ c ^ d; g = (3u * i + 5u) & 15u; }
        else { f = c ^ (b | ~d); g = (7u * i) & 15u; }
        t = d; d = c; c = b;
        b = b + DQ8_MD5_ROTL(a + f + dq8_md5_k[i] + w[g], dq8_md5_s[i]);
        a = t;
    }
    m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}
static void dq8_md5_init(Dq8Md5 *m) {
    m->h[0] = 0x67452301u; m->h[1] = 0xefcdab89u;
    m->h[2] = 0x98badcfeu; m->h[3] = 0x10325476u; m->len = 0;
}
static void dq8_md5_update(Dq8Md5 *m, const uint8_t *p, size_t n) {
    size_t have = (size_t)(m->len & 63u), i = 0;
    m->len += (uint64_t)n;
    if (have) {
        size_t take = 64u - have < n ? 64u - have : n;
        memcpy(m->buf + have, p, take);
        if (have + take < 64u) return;
        dq8_md5_block(m, m->buf); i = take;
    }
    for (; i + 64u <= n; i += 64u) dq8_md5_block(m, p + i);
    memcpy(m->buf, p + i, n - i);
}
static void dq8_md5_final(Dq8Md5 *m, uint8_t out[16]) {
    uint64_t bits = m->len * 8u;
    size_t have = (size_t)(m->len & 63u);
    uint8_t pad[72];
    size_t pad_len = have < 56u ? 56u - have : 120u - have;
    memset(pad, 0, sizeof pad); pad[0] = 0x80u;
    dq8_md5_update(m, pad, pad_len);
    for (unsigned i = 0; i < 8u; ++i) pad[i] = (uint8_t)(bits >> (8u * i));
    dq8_md5_update(m, pad, 8u);
    for (unsigned i = 0; i < 4u; ++i)
        for (unsigned j = 0; j < 4u; ++j)
            out[i*4u+j] = (uint8_t)(m->h[i] >> (8u * j));
}
static int dq8_det_md5_file(const char *path, char hex[33], uint64_t *bytes_out) {
    FILE *f = fopen(path, "rb");
    Dq8Md5 m; uint8_t buf[65536], digest[16];
    size_t n; uint64_t total = 0;
    static const char hx[] = "0123456789abcdef";
    if (!f) return 0;
    dq8_md5_init(&m);
    while ((n = fread(buf, 1u, sizeof buf, f)) > 0u) { dq8_md5_update(&m, buf, n); total += n; }
    fclose(f);
    dq8_md5_final(&m, digest);
    for (unsigned i = 0; i < 16u; ++i) { hex[i*2u] = hx[digest[i] >> 4]; hex[i*2u+1u] = hx[digest[i] & 15u]; }
    hex[32] = 0;
    *bytes_out = total;
    return 1;
}
static const char *dq8_det_ppm_path(void) {
    static char path[512];
    const char *dir = getenv("DQ8_CAPTURE_PPM_DIR");
    if (dir && dir[0]) snprintf(path, sizeof path, "%s/dq8-screen.ppm", dir);
    else snprintf(path, sizeof path, "/tmp/dq8-screen.ppm");
    return path;
}
/* DETERMINISTIC (t_115d297c): digest a guest-RAM range, so an A/B can tell
   "the guest state is identical" from "the host presentation differed". */
static int dq8_det_md5_mem(uint32_t addr, uint64_t len, char hex[33]) {
    Dq8Md5 m; uint8_t digest[16];
    static const char hx[] = "0123456789abcdef";
    uint32_t mapped = 0;
    if (!guest_memory_range(addr, len, &mapped)) return 0;
    dq8_md5_init(&m);
    dq8_md5_update(&m, ram + mapped, (size_t)len);
    dq8_md5_final(&m, digest);
    for (unsigned i = 0; i < 16u; ++i) { hex[i*2u] = hx[digest[i] >> 4]; hex[i*2u+1u] = hx[digest[i] & 15u]; }
    hex[32] = 0;
    return 1;
}
/* Rebase the restored thread deadlines onto the virtual clock.  A savestate
   stores the whole GuestThread array, and `wake_ns` in it is an ABSOLUTE host
   CLOCK_MONOTONIC value from the run that saved it (host uptime, ~10^13 ns).
   The virtual clock starts at 1 s, so a restored deadline would sit thousands
   of virtual seconds in the future and the thread would never wake.  Both
   A/B runs load the same state, so replacing the unknown remaining sleep with
   one vblank period is deterministic and cannot stall. */
static void dq8_det_rebase_threads(void) {
    uint64_t now;
    if (!dq8_det_mode) return;
    now = dq8_guest_now_ns();
    for (unsigned i = 0; i < guest_thread_count && i < 64u; ++i) {
        GuestThread *t = &guest_threads[i];
        if (t->wake_ns) t->wake_ns = now + 16666667ull; /* one vblank period */
        if (t->blocked) t->block_since_ns = now;
        else t->block_since_ns = 0;
    }
    fprintf(stderr, "DETERMINISTIC: rebased %u restored thread deadlines onto the virtual clock\n",
            guest_thread_count);
}
/* One machine-readable line per deterministic run: the frame digest the A/B
   compares, the virtual clock the run ended on, and how the clock got there. */
static void dq8_det_report(void) {
    char hex[33]; uint64_t bytes = 0;
    if (!dq8_det_mode) return;
    if (dq8_det_md5_file(dq8_det_ppm_path(), hex, &bytes))
        fprintf(stderr, "DET_FRAME md5=%s bytes=%llu path=%s\n",
                hex, (unsigned long long)bytes, dq8_det_ppm_path());
    else
        fprintf(stderr, "DET_FRAME md5=missing path=%s\n", dq8_det_ppm_path());
    fprintf(stderr,
            "DET_CLOCK retired=%llu idle_ms=%.3f vclock_ms=%.3f vblanks=%llu "
            "idle_jumps=%llu ns_per_step=%llu audio_frames=%llu\n",
            (unsigned long long)dq8_det_steps_total,
            (double)dq8_det_idle_ns / 1e6,
            (double)(dq8_guest_now_ns() - dq8_det_base_ns) / 1e6,
            (unsigned long long)dq8_det_vblanks,
            (unsigned long long)dq8_det_idle_jumps,
            (unsigned long long)dq8_det_ns_per_step,
            (unsigned long long)dq8_det_audio_frames);
    {   /* guest-visible output digests: same state must produce the same RAM */
        char rh[33];
        if (dq8_det_md5_mem(0x1F300000u, 240u * 400u * 3u, rh))
            fprintf(stderr, "DET_RAM scanout_1F300000 md5=%s\n", rh);
        if (dq8_det_md5_mem(0x1F346500u, 240u * 400u * 3u, rh))
            fprintf(stderr, "DET_RAM scanout_1F346500 md5=%s\n", rh);
        if (pica_rt_w && pica_rt_h &&
            dq8_det_md5_mem(pica_rt_addr, (uint64_t)pica_rt_w * pica_rt_h * 4u, rh))
            fprintf(stderr, "DET_RAM rt_%08X md5=%s\n", pica_rt_addr, rh);
    }
    fflush(stderr);
}
/* TEMPO (t_3463e625): guest-pace accounting.  DQ8_TEMPO=1 prints one line per
   wall second, DQ8_TEMPO=2 also one line per PDC0 scan-out.  The point is to
   attribute "the game plays N x too slow" with numbers instead of guesses, so
   two independent notions of guest time are tracked side by side:
     guest_s  = (last value svcGetSystemTick returned) / 268111856
                - the guest's own clock: CLOCK_MONOTONIC here, so tick_ratio
                  below must stay ~1.000; anything else is a broken tick shim.
     pdc0_s   = PDC0 vblank deliveries / 60
                - the frame clock the guest's own logic advances on; it only
                  tracks wall time if vblanks are delivered on a wall-clock
                  chain instead of being re-anchored at each guest wait.
   vbl_resync counts vblank periods the F5 chain had to drop when it fell
   behind (> GSP_VBLANK_CATCHUP_CAP), cap_per_s counts display-transfer
   producer events (guest-rendered frames), sleep_per_s/sleep_ms_per_s show
   whether guest threads park on fixed-length sleeps.  Env-gated, default off:
   each hook is one cached-flag test plus increments. */
static int tempo_mode = -1;
static uint64_t tempo_t0_ns, tempo_last_line_ns, tempo_last_frame_ns;
static unsigned long long tempo_pdc0_total, tempo_pdc0_win;
static unsigned long long tempo_cap_total, tempo_cap_win;
static unsigned long long tempo_vbl_total, tempo_vbl_win, tempo_vbl_resync;
static unsigned long long tempo_tick_calls, tempo_tick_win, tempo_tick_last;
static unsigned long long tempo_tick_first;
static int tempo_tick_first_seen;
static unsigned long long tempo_tick_pc[8], tempo_tick_hits[8];
static unsigned long long tempo_sleep_calls, tempo_sleep_win, tempo_sleep_ns_win;
static unsigned long long tempo_sleep_hist_ns[8], tempo_sleep_hist_hits[8];
/* Per-handle wait accounting: which event the guest's frame/logic loop blocks
   on, how many times per second it completes ready (a logic iteration) and how
   long each park actually lasts.  Without this the "logic runs slower than the
   frame rate" report cannot be attributed to a specific wait. */
#define TEMPO_WAIT_SLOTS 8
static uint32_t tempo_wait_handle[TEMPO_WAIT_SLOTS];
static unsigned long long tempo_wait_calls[TEMPO_WAIT_SLOTS];
static unsigned long long tempo_wait_win[TEMPO_WAIT_SLOTS];
static unsigned long long tempo_wait_done[TEMPO_WAIT_SLOTS];
static unsigned long long tempo_wait_park_ns[TEMPO_WAIT_SLOTS];
static unsigned long long tempo_wait_last_ns[TEMPO_WAIT_SLOTS];
static unsigned long long tempo_wait_span_ns[TEMPO_WAIT_SLOTS];
static uint32_t tempo_wait_pc[TEMPO_WAIT_SLOTS];
static uint32_t tempo_wait_tid[TEMPO_WAIT_SLOTS];
static unsigned long long tempo_gsp_drops, tempo_gsp_drop_win;
static int tempo_wait_slot(uint32_t handle) {
    int free_slot = -1;
    for (int i = 0; i < TEMPO_WAIT_SLOTS; ++i) {
        if (tempo_wait_handle[i] == handle) return i;
        if (!tempo_wait_handle[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) { tempo_wait_handle[free_slot] = handle; return free_slot; }
    return -1;
}
static int tempo_get(void) {
    if (tempo_mode < 0) {
        const char *e = getenv("DQ8_TEMPO");
        tempo_mode = (e && e[0] && e[0] != '0') ? (e[0] == '2' ? 2 : 1) : 0;
        if (tempo_mode) {
            tempo_t0_ns = host_monotonic_ns();
            tempo_last_line_ns = tempo_t0_ns;
            tempo_last_frame_ns = tempo_t0_ns;
        }
    }
    return tempo_mode;
}
static void tempo_tick_seen(uint64_t ticks, uint32_t pc) {
    if (!tempo_get()) return;
    ++tempo_tick_calls; ++tempo_tick_win;
    tempo_tick_last = ticks;
    for (unsigned i = 0; i < 8u; ++i) {
        if (!tempo_tick_pc[i] || tempo_tick_pc[i] == pc) {
            if (!tempo_tick_pc[i]) tempo_tick_pc[i] = pc;
            ++tempo_tick_hits[i];
            return;
        }
    }
}
static void tempo_sleep_seen(uint64_t ns) {
    if (!tempo_get()) return;
    ++tempo_sleep_calls; ++tempo_sleep_win; tempo_sleep_ns_win += ns;
    for (unsigned i = 0; i < 8u; ++i) {
        if (!tempo_sleep_hist_ns[i] || tempo_sleep_hist_ns[i] == ns) {
            if (!tempo_sleep_hist_ns[i]) tempo_sleep_hist_ns[i] = ns;
            ++tempo_sleep_hist_hits[i];
            return;
        }
    }
}
static void tempo_vblank_seen(unsigned fired, unsigned resync) {
    if (!tempo_get()) return;
    tempo_vbl_total += fired; tempo_vbl_win += fired; tempo_vbl_resync += resync;
}
static void tempo_capture_seen(void) {
    if (!tempo_get()) return;
    ++tempo_cap_total; ++tempo_cap_win;
}
static int tempo_wait_enter(uint32_t handle) {
    int slot;
    if (!tempo_get()) return -1;
    slot = tempo_wait_slot(handle);
    if (slot < 0) return -1;
    tempo_wait_pc[slot] = current_guest_pc;
    tempo_wait_tid[slot] = current_guest_thread_id;
    ++tempo_wait_calls[slot];
    ++tempo_wait_win[slot];
    return slot;
}
static void tempo_wait_ready(int slot) {
    uint64_t now;
    if (slot < 0) return;
    now = host_monotonic_ns();
    if (tempo_wait_last_ns[slot] && now > tempo_wait_last_ns[slot])
        tempo_wait_span_ns[slot] += now - tempo_wait_last_ns[slot];
    tempo_wait_last_ns[slot] = now;
    ++tempo_wait_done[slot];
}
static void tempo_wait_park(int slot, uint64_t ns) {
    if (slot < 0) return;
    tempo_wait_park_ns[slot] += ns;
}
static void tempo_gsp_drop_seen(void) {
    if (!tempo_get()) return;
    ++tempo_gsp_drops; ++tempo_gsp_drop_win;
}
static unsigned long long tempo_swaps, tempo_swap_win;
static void tempo_swap_seen(void) {
    if (!tempo_get()) return;
    ++tempo_swaps; ++tempo_swap_win;
}
static void tempo_frame_seen(void) {   /* one PDC0 scan-out = one guest frame */
    uint64_t now;
    if (!tempo_get()) return;
    if (!tempo_tick_first_seen) {
        tempo_tick_first_seen = 1;
        tempo_tick_first = tempo_tick_last;
    }
    ++tempo_pdc0_total; ++tempo_pdc0_win;
    now = host_monotonic_ns();
    if (tempo_mode == 2)
        fprintf(stderr,
                "TEMPO-F f=%llu wall_s=%.4f dt_ms=%.2f pdc0_s=%.4f tick_s=%.4f\n",
                tempo_pdc0_total, (double)(now - tempo_t0_ns) / 1e9,
                (double)(now - tempo_last_frame_ns) / 1e6,
                (double)tempo_pdc0_total / 60.0,
                (double)(tempo_tick_last - tempo_tick_first) / 268111856.0);
    tempo_last_frame_ns = now;
    if (now - tempo_last_line_ns < 1000000000ull) return;
    {
        double wall_s = (double)(now - tempo_last_line_ns) / 1e9;
        double wall_total_s = (double)(now - tempo_t0_ns) / 1e9;
        double guest_s = (double)(tempo_tick_last - tempo_tick_first) / 268111856.0;
        double pdc0_s = (double)tempo_pdc0_total / 60.0;
        fprintf(stderr,
                "TEMPO wall_s=%.3f guest_s=%.3f tick_ratio=%.3f pdc0=%llu "
                "pdc0_per_s=%.2f pdc0_s=%.3f vblank_ratio=%.3f cap_per_s=%.1f "
                "vbl_per_s=%.2f vbl_total=%llu vbl_resync=%llu gsp_drops=%llu "
                "swap_per_s=%.2f swap_ratio30=%.3f "
                "tick_per_s=%.1f sleep_per_s=%.1f sleep_ms_per_s=%.1f\n",
                wall_total_s, guest_s,
                wall_total_s > 0.0 ? guest_s / wall_total_s : 0.0,
                tempo_pdc0_total,
                wall_s > 0.0 ? (double)tempo_pdc0_win / wall_s : 0.0,
                pdc0_s,
                wall_total_s > 0.0 ? pdc0_s / wall_total_s : 0.0,
                wall_s > 0.0 ? (double)tempo_cap_win / wall_s : 0.0,
                wall_s > 0.0 ? (double)tempo_vbl_win / wall_s : 0.0,
                tempo_vbl_total, tempo_vbl_resync, tempo_gsp_drops,
                wall_s > 0.0 ? (double)tempo_swap_win / wall_s : 0.0,
                wall_s > 0.0 ? (double)tempo_swap_win / wall_s / 30.0 : 0.0,
                wall_s > 0.0 ? (double)tempo_tick_win / wall_s : 0.0,
                wall_s > 0.0 ? (double)tempo_sleep_win / wall_s : 0.0,
                wall_s > 0.0 ? (double)tempo_sleep_ns_win / 1e6 / wall_s : 0.0);
        for (unsigned i = 0; i < TEMPO_WAIT_SLOTS; ++i)
            if (tempo_wait_win[i])
                fprintf(stderr,
                        "TEMPO-WAIT handle=%08X calls_per_s=%.2f ready_per_s=%.2f "
                        "avg_ready_gap_ms=%.3f pc=%08X thread=%08X\n",
                        tempo_wait_handle[i],
                        wall_s > 0.0 ? (double)tempo_wait_win[i] / wall_s : 0.0,
                        wall_s > 0.0 ? (double)tempo_wait_done[i] / wall_s : 0.0,
                        tempo_wait_done[i]
                            ? (double)tempo_wait_span_ns[i] / 1e6 /
                              (double)tempo_wait_done[i]
                            : 0.0,
                        tempo_wait_pc[i], tempo_wait_tid[i]);
        for (unsigned i = 0; i < 8u; ++i)
            if (tempo_tick_hits[i])
                fprintf(stderr,
                        "TEMPO-PC tick_pc=%08X tick_hits=%llu sleep_ns=%llu "
                        "sleep_hits=%llu\n",
                        (unsigned)tempo_tick_pc[i], tempo_tick_hits[i],
                        tempo_sleep_hist_ns[i], tempo_sleep_hist_hits[i]);
    }
    tempo_pdc0_win = tempo_cap_win = tempo_vbl_win = 0;
    tempo_tick_win = tempo_sleep_win = tempo_sleep_ns_win = 0;
    tempo_gsp_drop_win = 0;
    tempo_swap_win = 0;
    for (unsigned i = 0; i < TEMPO_WAIT_SLOTS; ++i) {
        tempo_wait_win[i] = 0;
        tempo_wait_done[i] = 0;
        tempo_wait_park_ns[i] = 0;
        tempo_wait_span_ns[i] = 0;
    }
    tempo_last_line_ns = now;
}
/* PERF-02 honest benchmark counters.  The §5.1 harness reported only the
   main context's `steps`, so worker (decoder/audio) retirement was invisible
   and a session that spent its time in workers looked like "no guest
   progress".  These counters keep the three quantities apart: main context,
   worker contexts, and their aggregate, plus the dispatch/wake bookkeeping
   that explains where wall time went.  Retired counts are committed when a
   dispatch returns (see translated_from), never per instruction, so the hot
   loop pays nothing. */
static uint64_t perf_main_retired;      /* retired instructions, main guest context */
static uint64_t perf_worker_retired;    /* retired instructions, all worker contexts */
static uint64_t perf_main_dispatches;   /* translated_from invocations, main context */
static uint64_t perf_worker_dispatches; /* translated_from invocations, workers */
static uint64_t perf_main_sleeps;       /* main SleepThread calls with duration != 0 */
static uint64_t perf_main_sleep_ns;     /* requested main sleep durations (ns) */
static uint64_t perf_worker_sleeps;     /* worker SleepThread calls with duration != 0 */
static uint64_t perf_worker_parks;      /* worker waits/sleeps parked on the scheduler */
static uint64_t perf_main_wait_spins;   /* main-context wait service iterations */
/* PERF-02 scene markers: what the measured segment actually reached.  A run
   that never enters the movie player and never produces a Y2R conversion is
   the first-run save modal / startup path, not the intro; the bench harness
   must be able to say so instead of projecting it as the intro. */
static uint64_t perf_movie_entries;      /* guest entered 0x002FA318 (play-and-wait) */
static uint64_t perf_y2r_conversions;    /* completed Y2R StartConversion service calls */
static uint64_t perf_dsp_ticks;          /* dsp_audio_tick invocations with audio on */
static uint64_t perf_dsp_resyncs;        /* audio deadline misses (backlog resynced) */
static uint64_t perf_host_yields;       /* host sched_yield() calls on guest behalf */
static uint64_t perf_host_sleeps;       /* host nanosleep() calls on guest behalf */
static uint64_t perf_host_sleep_ns;     /* host nanosleep() time requested */
static void guest_host_yield(void) {
    ++perf_host_yields;
    sched_yield();
}
/* Bounded host nap used while a guest context waits.  The cap keeps a caller
   that asks for an enormous (or negative, i.e. indefinite) duration from
   parking the whole runtime inside one host sleep: the wait loops re-check
   their deadline and keep servicing the frame/audio/HID cadence. */
static void guest_host_sleep_ns(uint64_t ns) {
    if (ns > 20000000ull) ns = 20000000ull;
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    ++perf_host_sleeps;
    perf_host_sleep_ns += ns;
    nanosleep(&ts, NULL);
}
/* DQ8_TRACE_MOVIE: bounded movie-pipeline pacing trace (F3, task t_c6ea81c4).
   Env-gated (default off -> zero behaviour change): timestamped pipeline
   events (RomFS reads, wait parks/wakes with reason, sleeps, Y2R conversions)
   plus a 5 s per-thread report of retired-vs-parked wall time.  This answers
   the audit H1 question - where the movie frame pipeline actually waits -
   without touching guest-visible semantics. */
#include <stdarg.h>
static int movie_trace_enabled = -1;
static int movie_trace_events_movie_only;   /* mode 2: log events after movie entry */
static uint64_t movie_trace_t0_ns, movie_trace_next_report_ns;
static uint64_t movie_trace_events;
static uint64_t movie_trace_event_cap = 8000u;
static uint64_t movie_trace_fs_reads, movie_trace_fs_bytes;
static uint64_t movie_trace_sleeps, movie_trace_sleep_ns, movie_trace_sleep_max_ns;
static uint64_t movie_trace_parks, movie_trace_wake_signal, movie_trace_wake_timeout,
               movie_trace_wake_ready, movie_trace_wake_other;
static uint64_t movie_trace_y2r_count, movie_trace_y2r_ns, movie_trace_y2r_max_ns;
static const char *movie_trace_wake_reason;
static int movie_trace(void) {
    if (movie_trace_enabled < 0) {
        const char *value = getenv("DQ8_TRACE_MOVIE");
        movie_trace_enabled = (value && *value && *value != '0') ? 1 : 0;
        if (movie_trace_enabled) {
            /* Mode 2 (DQ8_TRACE_MOVIE=2): per-event log starts at the first
               movie-player entry and has a 100k cap, so the event stream
               covers the movie window instead of the startup/dialog phase. */
            if (value[0] == '2') {
                movie_trace_events_movie_only = 1;
                movie_trace_event_cap = 100000u;
            }
            movie_trace_t0_ns = host_monotonic_ns();
            fprintf(stderr, "MOVIE_TRACE start t_ns=%llu mode=%d\n",
                    (unsigned long long)movie_trace_t0_ns, value[0] == '2' ? 2 : 1);
        }
    }
    return movie_trace_enabled;
}
static void movie_trace_event(const char *fmt, ...) {
    uint64_t now;
    va_list ap;
    if (!movie_trace()) return;
    if (movie_trace_events_movie_only && !perf_movie_entries) return;
    if (movie_trace_events >= movie_trace_event_cap) return;
    ++movie_trace_events;
    if (movie_trace_events == movie_trace_event_cap)
        fprintf(stderr, "MOVIE_TRACE event cap %llu reached; aggregates continue\n",
                (unsigned long long)movie_trace_event_cap);
    now = host_monotonic_ns();
    fprintf(stderr, "MOVIE_TRACE t_ms=%llu ",
            (unsigned long long)((now - movie_trace_t0_ns) / 1000000ull));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
static void movie_trace_report(const char *why) {
    static uint64_t last_retired[64], last_blocked_ns[64], last_parks[64];
    static uint64_t last_y2r, last_entries, last_fs_reads, last_sleeps,
                   last_parks_total, last_main_retired, last_worker_retired,
                   last_dsp_ticks, last_main_sleeps, last_main_sleep_ns,
                   last_host_sleeps;
    uint64_t now = host_monotonic_ns();
    uint64_t main_retired = perf_main_retired, worker_retired = perf_worker_retired;
    fprintf(stderr,
            "MOVIE_REPORT t_ms=%llu why=%s y2r=%llu entries=%llu fs_reads=%llu "
            "sleeps=%llu parks=%llu wake_sig=%llu wake_to=%llu wake_rdy=%llu wake_oth=%llu "
            "main_retired=%llu worker_retired=%llu dsp_ticks=%llu "
            "main_sleeps=%llu main_sleep_ms=%llu host_sleeps=%llu\n",
            (unsigned long long)((now - movie_trace_t0_ns) / 1000000ull), why,
            (unsigned long long)(perf_y2r_conversions - last_y2r),
            (unsigned long long)(perf_movie_entries - last_entries),
            (unsigned long long)(movie_trace_fs_reads - last_fs_reads),
            (unsigned long long)(movie_trace_sleeps - last_sleeps),
            (unsigned long long)(movie_trace_parks - last_parks_total),
            (unsigned long long)movie_trace_wake_signal,
            (unsigned long long)movie_trace_wake_timeout,
            (unsigned long long)movie_trace_wake_ready,
            (unsigned long long)movie_trace_wake_other,
            (unsigned long long)(main_retired - last_main_retired),
            (unsigned long long)(worker_retired - last_worker_retired),
            (unsigned long long)(perf_dsp_ticks - last_dsp_ticks),
            (unsigned long long)(perf_main_sleeps - last_main_sleeps),
            (unsigned long long)((perf_main_sleep_ns - last_main_sleep_ns) / 1000000ull),
            (unsigned long long)(perf_host_sleeps - last_host_sleeps));
    movie_trace_wake_signal = movie_trace_wake_timeout = 0;
    movie_trace_wake_ready = movie_trace_wake_other = 0;
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        GuestThread *t = &guest_threads[i];
        fprintf(stderr,
                "MOVIE_THREAD t_ms=%llu idx=%u id=%08X active=%d blocked=%d "
                "retired=%llu blocked_ms=%llu parks=%llu pc=%08X wait=%08X/%u/%08X "
                "wake_in_ms=%lld\n",
                (unsigned long long)((now - movie_trace_t0_ns) / 1000000ull),
                i, t->guest_thread_id, t->active, t->blocked,
                (unsigned long long)(t->retired_steps - last_retired[i]),
                (unsigned long long)((t->blocked_ns - last_blocked_ns[i]) / 1000000ull),
                (unsigned long long)(t->blocked_count - last_parks[i]),
                t->pc, t->wait_handle, t->wait_type, t->wait_value,
                t->wake_ns ? (long long)((t->wake_ns - now) / 1000000ull) : -1LL);
        last_retired[i] = t->retired_steps;
        last_blocked_ns[i] = t->blocked_ns;
        last_parks[i] = t->blocked_count;
    }
    last_y2r = perf_y2r_conversions;
    last_entries = perf_movie_entries;
    last_fs_reads = movie_trace_fs_reads;
    last_sleeps = movie_trace_sleeps;
    last_parks_total = movie_trace_parks;
    last_main_retired = main_retired;
    last_worker_retired = worker_retired;
    last_dsp_ticks = perf_dsp_ticks;
    last_main_sleeps = perf_main_sleeps;
    last_main_sleep_ns = perf_main_sleep_ns;
    last_host_sleeps = perf_host_sleeps;
    fflush(stderr);
}
static void movie_trace_tick(void) {
    uint64_t now;
    if (!movie_trace()) return;
    now = host_monotonic_ns();
    if (!movie_trace_next_report_ns) movie_trace_next_report_ns = now + 5000000000ull;
    else if (now >= movie_trace_next_report_ns) {
        movie_trace_next_report_ns = now + 5000000000ull;
        movie_trace_report("tick");
    }
}
static int guest_any_runnable_thread(void) {
    for (unsigned i = 0; i < guest_thread_count; ++i)
        if (guest_threads[i].active && !guest_threads[i].blocked) return 1;
    return 0;
}
/* DQ8 bench harness (perf audit 5.1): DQ8_BENCH_STEPS=<N> runs the main
   guest for exactly N guest instructions (the A05 `steps` accounting
   counter) and then reports wall time and guest instructions/sec before
   exiting 0.  The budget folds into the existing loop bound, so an unset
   variable keeps the previous 1e12 guard exactly and costs one global load
   per dispatch iteration. */
static uint64_t bench_step_limit = 1000000000000ull;
static uint64_t bench_irq_counts[8]; /* PERF-04 harness: GSP irq counters per PDC */
static int bench_enabled;
/* DQ8_BENCH_NO_SNAPSHOT=1 suppresses ONLY the diagnostic /tmp PPM snapshots
   (gsp_write_screen_ppms).  Default behaviour is unchanged: without the
   variable the snapshots are written exactly as before.  The switch exists so
   a benchmark can separate "guest work + presentation" from "writing 0.5 MB of
   debug PPM five times a second", which the §5.1 profile shows is a large part
   of startup CPU. */
static int bench_no_snapshot;
static uint64_t bench_snapshots;
static uint64_t bench_t0_ns;
static void bench_report(uint64_t steps, uint32_t pc) {
    uint64_t ns = host_monotonic_ns() - bench_t0_ns;
    double secs = (double)ns / 1e9;
    /* `steps` is the *current invocation's* local accounting, i.e. the main
       context only, and at the bench boundary it already counts the
       instruction being retired.  Report workers separately; the aggregate
       is the only number that describes guest throughput. */
    uint64_t main_steps = perf_main_retired + steps;
    uint64_t worker_steps = perf_worker_retired;
    uint64_t aggregate = main_steps + worker_steps;
    uint64_t dispatch_calls = perf_main_dispatches + perf_worker_dispatches;
    fprintf(stderr,
            "BENCH steps=%llu pc=%08X wall=%.3f sec instr_per_sec=%.3fM snapshots=%llu no_snapshot=%d"
            " main_steps=%llu worker_steps=%llu aggregate_steps=%llu aggregate_instr_per_sec=%.3fM"
            " main_dispatches=%llu worker_dispatches=%llu dispatch_calls=%llu"
            " idle_main_sleeps=%llu idle_main_sleep_ms=%.3f idle_worker_sleeps=%llu"
            " idle_main_wait_spins=%llu idle_worker_parks=%llu host_yields=%llu host_sleeps=%llu"
            " scene_movie_entries=%llu scene_y2r_conversions=%llu worker_threads=%u"
            " audio_ticks=%llu audio_resyncs=%llu\n",
            (unsigned long long)steps, pc, secs,
            secs > 0.0 ? (double)steps / secs / 1e6 : 0.0,
            (unsigned long long)bench_snapshots, bench_no_snapshot,
            (unsigned long long)main_steps,
            (unsigned long long)worker_steps,
            (unsigned long long)aggregate,
            secs > 0.0 ? (double)aggregate / secs / 1e6 : 0.0,
            (unsigned long long)perf_main_dispatches,
            (unsigned long long)perf_worker_dispatches,
            (unsigned long long)dispatch_calls,
            (unsigned long long)perf_main_sleeps,
            (double)perf_main_sleep_ns / 1e6,
            (unsigned long long)perf_worker_sleeps,
            (unsigned long long)perf_main_wait_spins,
            (unsigned long long)perf_worker_parks,
            (unsigned long long)perf_host_yields,
            (unsigned long long)perf_host_sleeps,
            (unsigned long long)perf_movie_entries,
            (unsigned long long)perf_y2r_conversions,
            guest_thread_count,
            (unsigned long long)perf_dsp_ticks,
            (unsigned long long)perf_dsp_resyncs);
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        GuestThread *t = &guest_threads[i];
        fprintf(stderr, "BENCH_THREAD index=%u id=%08X active=%d blocked=%d retired=%llu dispatches=%llu sleeps=%llu\n",
                i, t->guest_thread_id, t->active, t->blocked,
                (unsigned long long)t->retired_steps,
                (unsigned long long)t->dispatch_calls,
                (unsigned long long)t->sleep_calls);
    }
    fflush(stderr);
    split_render_flush_sync(); /* present worker: final frame visible before exit */
    exit(0);
}
static float f32_from_bits(uint32_t bits) { float f; memcpy(&f, &bits, sizeof(f)); return f; }
static uint32_t f32_to_bits(float f) { uint32_t bits; memcpy(&bits, &f, sizeof(bits)); return bits; }
static double f64_from_bits(uint64_t bits) { double f; memcpy(&f, &bits, sizeof(f)); return f; }
static uint64_t f64_to_bits(double f) { uint64_t bits; memcpy(&bits, &f, sizeof(bits)); return bits; }
static int f32_is_nan_bits(uint32_t bits) { return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0; }
static int f64_is_nan_bits(uint64_t bits) { return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (bits & 0x000FFFFFFFFFFFFFull) != 0; }
static uint32_t next_mutex_handle = 0x10000u;
static uint8_t mutex_live[64], mutex_locked[64];
/* Explicit mutex ownership: mutex_owner is the guest thread id holding the
   lock (0 = free) and mutex_recursion is the holder's nesting count, the
   same owner/lock_count model Azahar's kernel mutex uses. */
static uint32_t mutex_owner[64];
static uint8_t mutex_recursion[64];
static uint32_t next_event_handle = 0x20u; /* skip service session range 5..16 */
static uint8_t event_live[256], event_signaled[256], event_manual_reset[256];
/* Files, directories and events share one kernel-handle namespace. Separate
   file counters overwrote the live RomFS handles when save files opened. */
static uint32_t guest_alloc_ipc_handle(void) {
    if (next_event_handle >= 256u) { fputs("native handle table exhausted\n", stderr); exit(22); }
    return next_event_handle++;
}
static uint32_t next_arbiter_handle = 0x20000u;
static uint8_t arbiter_live[64];
/* The main guest context runs on the host stack rather than in
   guest_threads[]. Keep its one in-flight arbiter wait visible to a peer's
   ArbitrateAddress(Signal) so main waits can park without losing the wake. */
static uint8_t main_arbiter_waiting, main_arbiter_signaled;
static uint32_t main_arbiter_handle, main_arbiter_address;
static uint8_t gsp_relay_registered;
static uint32_t gsp_relay_event;
static uint32_t gsp_shared_base;
static uint32_t gsp_thread_id;
static uint32_t gsp_lcd_force_black = 1u;
/* VBLANK WALLCLOCK (t_d1be62fc): published by the pacer thread, read by the
   guest-side delivery and the relay-thread vblank park. */
static _Atomic uint64_t gsp_next_vblank_ns;
static uint32_t dsp_pipe_interrupt_event;
/* Hold source-boundary DSP output until the guest acknowledges its manual-reset
   Pipe event with ClearEvent; otherwise a later frame can erase the one-shot
   SourceStatus update before the audio worker reads it. */
static int dsp_source_event_pending;
static uint16_t dsp_recv_data[16];
static uint8_t dsp_recv_ready[16];
static uint8_t dsp_audio_pipe[40];
static uint32_t dsp_audio_pipe_size, dsp_audio_pipe_pos;
/* DSP DRAM dumper (evidence helper for the audio track). DQ8_DSP_DUMP=<prefix>
   snapshots the 512 KiB DSP window 0x1FF00000..0x1FF7FFFF (shared-memory
   regions, source buffers, frame counters) at most every 2 s and once at
   exit, into <prefix>_<seq>.bin. */
static char dsp_dump_prefix[512];
static uint64_t dsp_dump_last_ns;
static unsigned dsp_dump_seq;
static void dsp_dump_now(const char *why) {
    char path[600];
    FILE *f;
    if (!dsp_dump_prefix[0]) return;
    snprintf(path, sizeof path, "%s_%04u.bin", dsp_dump_prefix, dsp_dump_seq++);
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "dsp dump: cannot write %s\n", path); return; }
    fwrite(ram + 0x1FF00000u, 1, 0x80000u, f);
    fclose(f);
    fprintf(stderr, "dsp dump: wrote %s (%s)\n", path, why);
}
static void dsp_dump_at_exit(void) { dsp_dump_now("exit"); }
static void dsp_dump_maybe(void) {
    uint64_t now;
    if (!dsp_dump_prefix[0]) {
        const char *env = getenv("DQ8_DSP_DUMP");
        if (!env || !env[0]) return;
        snprintf(dsp_dump_prefix, sizeof dsp_dump_prefix, "%s", env);
        atexit(dsp_dump_at_exit);
    }
    now = host_monotonic_ns();
    if (now - dsp_dump_last_ns < 2000000000ull) return;
    dsp_dump_last_ns = now;
    dsp_dump_now("tick");
}
/* ---- Minimal DSP audio HLE (the audio track) -------------------------------
   DQ8's audio library talks to the DSP through two shared-memory regions and
   the audio pipe.  This runtime now acts as that DSP, far enough to reproduce
   the sound the title plays while it creates save/extra data:

   * every audio frame (160 samples @ 32728 Hz) the registered audio-pipe
     interrupt event is signalled, which wakes the guest's audio worker —
     without it the worker blocks right after boot and no sound ever exists;
   * the source configuration the guest writes into the region it owns is
     parsed each frame (enable, pitch, gains, PCM8/PCM16/ADPCM buffers with
     looping and the ADPCM predictor state), the enabled sources are decoded
     from guest memory, pitch-resampled with linear interpolation, mixed with
     the master/aux volumes and clipped, and the result is written to the
     other region's Final Mix Samples block just like the real DSP;
   * the mixed stereo frame is pushed to the SDL2 audio backend and, when
     DQ8_AUDIO_DUMP=<path> is set, recorded into a WAV file.

   Intentionally NOT emulated (they do not take part in this stage's audio):
   delay/reverb effects, the compressor, per-source filters and polyphase
   interpolation.  Structure offsets follow Azahar's HLE SharedMemory layout,
   which is exactly the address table this runtime serves on the audio pipe.
   -------------------------------------------------------------------------- */
#if 0 /* replaced by the H1 SPSC-backed H2 audio workers */
#define DSP_RATE 32728u
#define DSP_FRAME_SAMPLES 160u
#define DSP_FRAME_NS 4888791ull /* 160 / 32728 s in nanoseconds */
#define DSP_REGION0 0x1FF50000u
#define DSP_REGION1 0x1FF70000u
#define DSP_OFF_STATUS 0x820u
#define DSP_OFF_FINAL 0x5224u
#define DSP_OFF_SRC_STATUS 0xA80u
/* This title's firmware places the DSP Configuration (dirty word +
   master/aux volumes + output_buffer_count + surround fields — the
   DspConfiguration struct of the local Azahar tree,
   audio_core/hle/shared_memory.h) at pipe addr 0x8710 = +0xE20, not at
   Azahar's exemplar 0x9430.  Verified against run19/20 DSP dumps: the
   guest wrote BF01BFC0 (dirty), 1.0f/1.0f/1.0f (master + two aux
   returns), output_buffer_count=3, and the +0x22..+0x28 halfwords
   (aux_front_bypass/aux_bus_enable) — field-for-field the same struct. */
#define DSP_OFF_APP_DSP_CONFIG 0xE20u
/* Pipe addr 0x9430 = +0x2860 is this firmware's Frame Counter (u16,
   even for region0 / odd for region1, first value 4), not the config. */
#define DSP_OFF_DSP_CONFIG 0x2860u
/* THIS firmware's middleware writes SourceConfiguration at DSP 0x8400 (= +0x800)
   and Source ADPCM Coefficients at 0x9492 (= +0x2924), NOT Azahar's exemplar
   0x9E92/0xA792.  Proven against run22 DSP dumps: at +0x800 there is a live
   SourceConfiguration (dirty 0x00010000 enable, stereo gains 0.974, rate 0.673,
   embedded ADPCM buffer phys 0x20366CE0 len 3259); at +0x3D24 it is all-zero.
   The frame counter (+0x2860=0x9430) and DSP Configuration (+0xE20=0x8710)
   below were already tuned to this firmware; these two complete the set. */
#define DSP_OFF_SRC_CONFIG 0x800u
#define DSP_OFF_COEFFS 0x2924u
#define DSP_OFF_FRAME_COUNTER 0x7FFEu
#define DSP_NUM_SOURCES 24u
#define DSP_MAX_DECODED 8000000u
static int guest_memory_range(uint32_t, uint64_t, uint32_t *);
static uint16_t dsp_r16(uint32_t a) {
    return (uint16_t)((uint32_t)ram[a] | ((uint32_t)ram[a + 1u] << 8));
}
static uint32_t dsp_r32(uint32_t a) {
    return (uint32_t)ram[a] | ((uint32_t)ram[a + 1u] << 8) |
           ((uint32_t)ram[a + 2u] << 16) | ((uint32_t)ram[a + 3u] << 24);
}
/* u32_dsp: the DSP is 16-bit big-endian, so 32-bit words appear
   middle-endian (halves swapped) from the ARM11 side. */
static uint32_t dsp_r32dsp(uint32_t a) {
    uint32_t v = dsp_r32(a);
    return (v << 16) | (v >> 16);
}
static float dsp_rfloat(uint32_t a) {
    uint32_t b = dsp_r32(a);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}
static void dsp_w16(uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)v;
    ram[a + 1u] = (uint8_t)(v >> 8);
}
static void dsp_w32(uint32_t a, uint32_t v) {
    ram[a] = (uint8_t)v;
    ram[a + 1u] = (uint8_t)(v >> 8);
    ram[a + 2u] = (uint8_t)(v >> 16);
    ram[a + 3u] = (uint8_t)(v >> 24);
}
static void dsp_w32dsp(uint32_t a, uint32_t v) { dsp_w32(a, (v << 16) | (v >> 16)); }

typedef struct {
    uint32_t addr, length, channels, format, looping, buffer_id;
    uint32_t play_position, adpcm_dirty;
    uint16_t ps, yn[2];
} DspPending;
typedef struct {
    int enabled;
    float rate;
    float gain[3][4];
    uint16_t sync_count;
    /* current buffer: PCM reads straight from guest RAM, ADPCM is decoded
       into host memory when the buffer is dequeued */
    int16_t *decoded;
    uint32_t data_off;
    uint32_t sample_count;
    uint32_t channels, format, looping;
    uint32_t pos;
    float frac;
    uint16_t current_buffer_id, last_buffer_id;
    int buffer_update;
    DspPending pending[8];
    unsigned pending_count;
} DspSourceState;
static DspSourceState dsp_src[DSP_NUM_SOURCES];
static int dsp_audio_on;              /* DSP state: set by the audio-pipe state change */
static uint64_t dsp_next_frame_ns;
static uint64_t dsp_frames_out;
static uint64_t dsp_sources_started, dsp_sources_finished;
static float dsp_master_volume, dsp_aux_volume[2];

/* WAV capture (DQ8_AUDIO_DUMP=<path>): header is refreshed periodically so an
   abruptly terminated run still leaves a playable file. */
static FILE *dsp_wav_file;
static uint64_t dsp_wav_frames;
static char dsp_wav_path[512];
#ifdef DQ8_ENABLE_SDL
static unsigned dsp_sdl_dev;
#endif
static int dsp_out_init_done;
static void dsp_wav_update_header(void) {
    uint8_t h[44];
    uint32_t bytes = (uint32_t)(dsp_wav_frames * 4u), riff = 36u + bytes, i;
    memcpy(h, "RIFF", 4);
    for (i = 0; i < 4; ++i) h[4u + i] = (uint8_t)(riff >> (8 * i));
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;   /* fmt chunk size */
    h[20] = 1; h[21] = 0;                           /* PCM */
    h[22] = 2; h[23] = 0;                           /* stereo */
    h[24] = (uint8_t)DSP_RATE; h[25] = (uint8_t)(DSP_RATE >> 8);
    h[26] = 0; h[27] = 0;                           /* sample rate */
    { uint32_t br = DSP_RATE * 4u;                  /* byte rate */
      h[28] = (uint8_t)br; h[29] = (uint8_t)(br >> 8);
      h[30] = (uint8_t)(br >> 16); h[31] = (uint8_t)(br >> 24);
      h[32] = 4; h[33] = 0;                         /* block align */
      h[34] = 16; h[35] = 0; }                      /* bits per sample */
    memcpy(h + 36, "data", 4);
    for (i = 0; i < 4; ++i) h[40u + i] = (uint8_t)(bytes >> (8 * i));
    if (fseek(dsp_wav_file, 0, SEEK_SET) == 0) {
        fwrite(h, 1, sizeof(h), dsp_wav_file);
        fseek(dsp_wav_file, 0, SEEK_END);
    }
}
static void dsp_audio_out_finish(void) {
    if (dsp_wav_file) {
        dsp_wav_update_header();
        fclose(dsp_wav_file);
        dsp_wav_file = NULL;
    }
}
static void dsp_audio_out_init(void) {
    const char *wav;
    if (dsp_out_init_done) return;
    dsp_out_init_done = 1;
    wav = getenv("DQ8_AUDIO_DUMP");
    if (wav && wav[0]) {
        snprintf(dsp_wav_path, sizeof dsp_wav_path, "%s", wav);
        dsp_wav_file = fopen(dsp_wav_path, "wb");
        if (dsp_wav_file) {
            uint8_t zero[44] = {0};
            fwrite(zero, 1, sizeof(zero), dsp_wav_file);
            atexit(dsp_audio_out_finish);
            fprintf(stderr, "host audio: recording %u Hz stereo WAV to %s\n", DSP_RATE, dsp_wav_path);
        } else {
            fprintf(stderr, "host audio: cannot open %s for writing\n", dsp_wav_path);
        }
    }
#ifdef DQ8_ENABLE_SDL
    {
        const char *nosdl = getenv("DQ8_AUDIO_SDL");
        SDL_AudioSpec want, have;
        if (!(nosdl && nosdl[0] == '0')) {
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
                fprintf(stderr, "host audio: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
            } else {
                memset(&want, 0, sizeof(want));
                want.freq = (int)DSP_RATE;
                want.format = AUDIO_S16SYS;
                want.channels = 2;
                want.samples = 480;
                dsp_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
                if (!dsp_sdl_dev) {
                    fprintf(stderr, "host audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
                } else {
                    SDL_PauseAudioDevice(dsp_sdl_dev, 0);
                    fprintf(stderr, "host audio: SDL2 output %d Hz %u ch\n", have.freq, have.channels);
                }
            }
        }
    }
#endif
}
static void dsp_audio_out_push(const int16_t *pcm) {
    if (!dsp_out_init_done) dsp_audio_out_init();
    if (dsp_wav_file) {
        fwrite(pcm, 4u, DSP_FRAME_SAMPLES, dsp_wav_file);
        dsp_wav_frames++;
        if ((dsp_frames_out & 0xFFu) == 0u) dsp_wav_update_header();
    }
#ifdef DQ8_ENABLE_SDL
    if (dsp_sdl_dev) {
        /* Drop frames when the host queue is already a second deep so a slow
           or dummy sink cannot grow the queue without bound. */
        if (SDL_GetQueuedAudioSize(dsp_sdl_dev) <= DSP_RATE * 4u)
            SDL_QueueAudio(dsp_sdl_dev, pcm, DSP_FRAME_SAMPLES * 4u);
    }
#endif
    dsp_frames_out++;
}

static int16_t dsp_fetch(const DspSourceState *s, uint32_t frame, unsigned ch) {
    uint32_t idx = frame * s->channels + ch;
    if (frame >= s->sample_count) return 0;
    if (s->decoded) return s->decoded[idx];
    if (s->format == 0u) /* PCM8: unsigned byte scaled to s16 */
        return (int16_t)((uint16_t)ram[s->data_off + idx] << 8);
    return (int16_t)dsp_r16(s->data_off + idx * 2u);
}
/* GC-style ADPCM (14 samples per 8-byte frame), per Azahar's Codec::DecodeADPCM. */
static int16_t *dsp_decode_adpcm(uint32_t off, uint32_t length, const int16_t *coeff,
                                 uint16_t ps, const uint16_t *yn) {
    static const int nibble_signed[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};
    uint32_t frames = (length + 13u) / 14u, fi, i;
    uint32_t total = frames * 14u;
    int16_t *out;
    int yn1 = yn[0], yn2 = yn[1], scale = 1 << (ps & 0xF), idx = (ps >> 4) & 0x7;
    if (total > DSP_MAX_DECODED) return NULL;
    out = (int16_t *)malloc(total * sizeof(int16_t));
    if (!out) return NULL;
    for (fi = 0; fi < frames; ++fi) {
        uint32_t base = off + fi * 8u;
        int header = ram[base];
        int fr_scale = 1 << (header & 0xF), fr_idx = (header >> 4) & 0x7;
        (void)scale; (void)idx;
        for (i = 0; i < 14u && fi * 14u + i < length; ++i) {
            uint8_t byte = ram[base + 1u + i / 2u];
            int nib = nibble_signed[(i & 1u) ? (byte & 0xFu) : (byte >> 4)];
            int xn = nib * fr_scale;
            int val = ((xn << 11) + 0x400 + coeff[fr_idx * 2] * yn1 + coeff[fr_idx * 2 + 1] * yn2) >> 11;
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;
            yn2 = yn1;
            yn1 = val;
            out[fi * 14u + i] = (int16_t)val;
        }
    }
    return out;
}
static void dsp_free_buffer(DspSourceState *s) {
    if (s->decoded) { free(s->decoded); s->decoded = NULL; }
    s->sample_count = 0;
}
/* DQ8_DSP_SE_DIR=<dir>: on every dequeue write the buffer's raw bytes,
   its decode inputs (coeffs/ps/yn) and the decoded PCM so the mix can be
   reproduced offline.  Diagnostic only — no effect on the audio path. */
static void dsp_dump_dequeue(unsigned i, const DspPending *b, uint32_t read_region,
                             const int16_t *coeff) {
    static int checked;
    const char *dir;
    char path[640];
    FILE *f;
    uint32_t mapped = 0, raw_bytes, k;
    if (!checked) {
        checked = 1;
        dir = getenv("DQ8_DSP_SE_DIR");
        if (!dir || !dir[0]) checked = -1;
    }
    if (checked < 0) return;
    dir = getenv("DQ8_DSP_SE_DIR");
    raw_bytes = (b->format == 2u) ? ((b->length + 13u) / 14u) * 8u
               : b->length * b->channels * (b->format == 0u ? 1u : 2u);
    if (!guest_memory_range(b->addr & 0xFFFFFFFCu, raw_bytes, &mapped)) return;
    snprintf(path, sizeof path, "%s/src%02u_id%03u.bin", dir, i, b->buffer_id);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "src=%u addr=%08X len=%u ch=%u fmt=%u loop=%u id=%u pos=%u ps=%04X yn=%04X,%04X region=%08X\n",
            i, b->addr, b->length, b->channels, b->format, b->looping, b->buffer_id,
            b->play_position, b->ps, b->yn[0], b->yn[1], read_region);
    fprintf(f, "coeff");
    for (k = 0; k < 16; ++k) fprintf(f, " %04X", (uint16_t)coeff[k]);
    fprintf(f, "\nrate=%.6f master=%.4f aux=%.4f,%.4f\n", dsp_src[i].rate,
            dsp_master_volume, dsp_aux_volume[0], dsp_aux_volume[1]);
    fprintf(f, "gains");
    for (k = 0; k < 12; ++k) fprintf(f, " %.4f", dsp_src[i].gain[k / 4][k % 4]);
    fprintf(f, "\nraw_bytes=%u\n", raw_bytes);
    fclose(f);
    { /* append raw bytes after the text header block */
        FILE *g = fopen(path, "ab");
        if (g) {
            fwrite(ram + mapped, 1, raw_bytes, g);
            fclose(g);
        }
    }
    snprintf(path, sizeof path, "%s/src%02u_id%03u.dec", dir, i, b->buffer_id);
    f = fopen(path, "wb");
    if (f && dsp_src[i].decoded)
        fwrite(dsp_src[i].decoded, 2u, dsp_src[i].sample_count * dsp_src[i].channels, f);
    if (f) fclose(f);
    fprintf(stderr, "dsp dump: src=%u id=%u addr=%08X len=%u fmt=%u ch=%u dec=%u\n",
            i, b->buffer_id, b->addr, b->length, b->format, b->channels,
            dsp_src[i].decoded ? dsp_src[i].sample_count : 0u);
}
static int dsp_dequeue(unsigned i, uint32_t read_region) {
    DspSourceState *s = &dsp_src[i];
    DspPending b;
    uint32_t mapped, bytes;
    int16_t coeff[16];
    unsigned k;
    if (!s->pending_count) return 0;
    b = s->pending[0];
    memmove(&s->pending[0], &s->pending[1], (s->pending_count - 1u) * sizeof(DspPending));
    s->pending_count--;
    dsp_free_buffer(s);
    s->channels = b.channels;
    s->format = b.format;
    s->looping = b.looping;
    s->current_buffer_id = (uint16_t)b.buffer_id;
    s->last_buffer_id = 0;
    s->buffer_update = 1;
    s->pos = b.play_position;
    s->frac = 0.0f;
    for (k = 0; k < 16; ++k) coeff[k] = 0;
    if ((int32_t)b.length <= 0 || b.length > DSP_MAX_DECODED) return 1; /* skip garbage, stay idle */
    if (s->format == 2u) {
        /* ADPCM coefficients are static but the middleware only posts them
           (dirty bit 2) into the region it is producing into; the current
           read region may be the other one.  Read from whichever region
           actually carries them (coeff[0] of a real set is non-zero). */
        uint32_t coeff_off = read_region + DSP_OFF_COEFFS + i * 32u;
        uint32_t other = (read_region == DSP_REGION0) ? DSP_REGION1 : DSP_REGION0;
        for (k = 0; k < 16; ++k) coeff[k] = (int16_t)dsp_r16(coeff_off + k * 2u);
        if (coeff[0] == 0 && coeff[1] == 0) {
            for (k = 0; k < 16; ++k) coeff[k] = (int16_t)dsp_r16(other + DSP_OFF_COEFFS + i * 32u + k * 2u);
        }
        if (!guest_memory_range(b.addr & 0xFFFFFFFCu, (uint64_t)((b.length + 13u) / 14u) * 8u, &mapped))
            return 1;
        s->decoded = dsp_decode_adpcm(mapped, b.length, coeff, b.ps, b.yn);
        if (!s->decoded) return 1;
        s->sample_count = b.length;
    } else {
        bytes = (uint64_t)b.length * s->channels * (s->format == 0u ? 1u : 2u);
        if (!guest_memory_range(b.addr & 0xFFFFFFFCu, bytes, &mapped)) return 1;
        s->data_off = mapped;
        s->sample_count = b.length;
    }
    if (s->pos > s->sample_count) s->pos = 0;
    dsp_dump_dequeue(i, &b, read_region, coeff);
    dsp_sources_started++;
    return 1;
}
static void dsp_enqueue(unsigned i, const DspPending *b);
/* ------------------------------------------------------------------
   Plausibility gates for this firmware's SourceConfiguration traffic.

   Proven problem (run25/run26 WAV + run26 enqueue trace): the memory
   behind the UNUSED slots of the config array (slots 4..7 in both
   regions) carries other middleware data, and parsing both regions every
   frame read that data as configurations: dozens of bogus buffers per
   run (unaligned addresses like 0x00210021, lengths up to 4.1e9,
   format=3) and non-finite gains (e.g. -2.6e38 / NaN) were loaded into
   sources, and the junk that passed the dequeue guards was mixed into
   the final output — that is the full-scale clipping burst that follows
   the save-stage SE in every WAV (identical 176/464 clipped samples in
   run25 and run26, while the SE itself decodes clean).

   The real posting (source 0) passes every check: dirty word only uses
   defined bits, gains 0.9744 within the documented 0.0-1.0 range
   (Azahar shared_memory.h), rate 0.673735 = 22050/32728, word-aligned
   FCRAM buffer address 0x20366CE0, length 3259, format ADPCM.

   A rejected config is left completely untouched: no state change, no
   dirty-word clear (never corrupt the app's memory either).
   ------------------------------------------------------------------ */
static const char *dsp_config_reject_reason(uint32_t dirty, uint32_t cfg) {
    /* bits 5..15 and 31 are undefined in the SourceConfiguration ABI */
    if (dirty & 0x8000FFE0u) return "reserved dirty bits";
    if (dirty & ((1u << 25) | (1u << 26) | (1u << 27))) {
        unsigned k;
        for (k = 0; k < 12; ++k) {
            float g = dsp_rfloat(cfg + 4u + k * 4u);
            /* The firmware ABI documents [0,1], but this title's real streamed
               source-1 config writes gain 1.4173229.  The DSP/reference HLE
               applies the finite values as written; rejecting the whole config
               here drops its simultaneous enable + embedded/queued buffers.
               Keep a generous finite bound to reject stale-slot garbage and
               keep the int32 mix accumulator safely in range. */
            if (!isfinite(g) || g < -8.0f || g > 8.0f)
                return "gain outside finite safe range";
        }
    }
    if (dirty & (1u << 18)) {
        float r = dsp_rfloat(cfg + 52u);
        if (!(r > 0.0f) || r > 4.0f) return "rate multiplier out of range";
    }
    return NULL;
}
static const char *dsp_buffer_reject_reason(const DspPending *b) {
    /* Source buffers live in LINEAR/FCRAM/VRAM/DSP shared RAM (>= LINEAR
       alias).  Low ARM memory addresses are what this firmware's unused
       slot data produces (0x00210021, 0x00000000, ...) and never a real
       buffer; they would otherwise map into the flat image and play
       whatever bytes sit there. */
    if ((b->addr & 3u) != 0) return "unaligned address";
    if (b->addr < 0x14000000u) return "address below LINEAR";
    if ((int32_t)b->length <= 0 || b->length > DSP_MAX_DECODED) return "length out of range";
    if (b->format > 2u) return "format > ADPCM";
    if (b->channels == 0u || b->channels > 2u) return "bad channel count";
    {
        uint32_t mapped;
        uint64_t bytes = (b->format == 2u) ? ((uint64_t)b->length + 13u) / 14u * 8u
                           : (uint64_t)b->length * b->channels * (b->format == 0u ? 1u : 2u);
        if (!guest_memory_range(b->addr & 0xFFFFFFFCu, bytes, &mapped)) return "outside guest RAM";
    }
    return NULL;
}
static void dsp_enqueue(unsigned i, const DspPending *b) {
    DspSourceState *s = &dsp_src[i];
    const char *why = dsp_buffer_reject_reason(b);
    if (why) {
        static uint32_t last_rej[DSP_NUM_SOURCES];
        if (getenv("DQ8_TRACE_AUDIO") && last_rej[i] != b->buffer_id + 1u)
            fprintf(stderr, "dsp enqueue REJECTED: src=%u addr=%08X len=%u fmt=%u (%s)\n",
                    i, b->addr, b->length, b->format, why);
        last_rej[i] = b->buffer_id + 1u;
        return;
    }
    if (s->pending_count >= sizeof(s->pending) / sizeof(s->pending[0])) return;
    s->pending[s->pending_count++] = *b;
}
static void dsp_parse_source_config(unsigned i, uint32_t cfg) {
    DspSourceState *s = &dsp_src[i];
    uint32_t dirty = dsp_r32(cfg);
    if (!dirty) return;
    { /* reject mis-parsed slot data wholesale (see gate docs above) */
        static uint32_t last_rej[DSP_NUM_SOURCES][2];
        unsigned r = (cfg < DSP_REGION1) ? 0u : 1u;
        const char *why = dsp_config_reject_reason(dirty, cfg);
        if (why) {
            if (getenv("DQ8_TRACE_AUDIO") && last_rej[i][r] != dirty)
                fprintf(stderr, "dsp config REJECTED: src=%u region=%u dirty=%08X (%s)\n",
                        i, r, dirty, why);
            last_rej[i][r] = dirty;
            return; /* no state change, no dirty-word clear */
        }
        last_rej[i][r] = 0;
    }
    if (dirty & (1u << 29)) { /* reset */
        unsigned k;
        dsp_free_buffer(s);
        s->enabled = 0;
        s->pending_count = 0;
        s->buffer_update = 0;
        s->pos = 0; s->frac = 0;
        for (k = 0; k < 3; ++k) { unsigned j; for (j = 0; j < 4; ++j) s->gain[k][j] = 0.0f; }
    }
    if (dirty & (1u << 4)) s->pending_count = 0;  /* partial reset: drop queue */
    if (dirty & (1u << 16)) s->enabled = ram[cfg + 160u] != 0;
    if (dirty & (1u << 18)) {
        float r = dsp_rfloat(cfg + 52u);
        s->rate = (r > 0.0f) ? r : 1.0f;
    }
    if (dirty & (1u << 28)) s->sync_count = dsp_r16(cfg + 162u);
    if (dirty & ((1u << 25) | (1u << 26) | (1u << 27))) {
        unsigned k, j;
        for (k = 0; k < 3; ++k)
            for (j = 0; j < 4; ++j)
                s->gain[k][j] = dsp_rfloat(cfg + 4u + (k * 4u + j) * 4u);
    }
    if (dirty & (1u << 30)) { /* embedded buffer */
        DspPending b;
        uint16_t flags1 = dsp_r16(cfg + 180u), flags2 = dsp_r16(cfg + 188u);
        b.addr = dsp_r32dsp(cfg + 172u);
        b.length = dsp_r32dsp(cfg + 176u);
        b.channels = ((flags1 & 3u) == 2u) ? 2u : 1u;
        b.format = (flags1 >> 2) & 3u;
        b.looping = (flags2 >> 1) & 1u;
        b.buffer_id = dsp_r16(cfg + 190u);
        b.play_position = dsp_r32dsp(cfg + 164u);
        b.adpcm_dirty = (flags2 & 1u) ? 1u : 0u;
        b.ps = dsp_r16(cfg + 182u);
        b.yn[0] = dsp_r16(cfg + 184u);
        b.yn[1] = dsp_r16(cfg + 186u);
        if (getenv("DQ8_TRACE_AUDIO"))
            fprintf(stderr, "dsp enqueue: src=%u embedded addr=%08X len=%u ch=%u fmt=%u loop=%u id=%u pos=%u dirty2=%u\n",
                    i, b.addr, b.length, b.channels, b.format, b.looping, b.buffer_id,
                    b.play_position, b.adpcm_dirty);
        dsp_enqueue(i, &b);
    }
    if (dirty & (1u << 19)) { /* queued buffers */
        uint16_t bd = dsp_r16(cfg + 74u);
        uint16_t flags1 = dsp_r16(cfg + 180u);
        unsigned k;
        for (k = 0; k < 4u; ++k) if (bd & (1u << k)) {
            uint32_t base = cfg + 76u + k * 20u;
            DspPending b;
            b.addr = dsp_r32dsp(base);
            b.length = dsp_r32dsp(base + 4u);
            b.channels = ((flags1 & 3u) == 2u) ? 2u : 1u;
            b.format = (flags1 >> 2) & 3u;
            b.looping = ram[base + 15u];
            b.buffer_id = dsp_r16(base + 16u);
            b.play_position = 0;
            b.adpcm_dirty = ram[base + 14u];
            b.ps = dsp_r16(base + 8u);
            b.yn[0] = dsp_r16(base + 10u);
            b.yn[1] = dsp_r16(base + 12u);
            if (getenv("DQ8_TRACE_AUDIO"))
                fprintf(stderr, "dsp enqueue: src=%u queued%u addr=%08X len=%u loop=%u id=%u dirty2=%u\n",
                        i, k, b.addr, b.length, b.looping, b.buffer_id, b.adpcm_dirty);
            dsp_enqueue(i, &b);
        }
        dsp_w16(cfg + 74u, 0);
    }
    dsp_w32(cfg, 0); /* the DSP clears the dirty flags every frame */
}
static uint32_t dsp_current_region(void) {
    /* Standard protocol: the region with the higher frame counter (0xBFFF)
       is current.  This title's middleware, however, never writes that
       counter; it alternates the two regions with a 16-bit sequence counter
       in the low half of the DSP Configuration word (+0x2860, even for
       region0 and odd for region1 — run19 dumps: R0=0x049A growing,
       R1=0x0291; the run-time watch shows +2 per produced tick).  Prefer
       the frame counters when they differ, otherwise fall back to the
       sequence counter, and keep the previous pick while both are equal. */
    static uint32_t last_region = DSP_REGION1;
    uint16_t c0 = dsp_r16(DSP_REGION0 + DSP_OFF_FRAME_COUNTER);
    uint16_t c1 = dsp_r16(DSP_REGION1 + DSP_OFF_FRAME_COUNTER);
    if (c0 != c1) {
        if (c0 == 0xFFFFu && c1 != 0xFFFEu) last_region = DSP_REGION1;
        else if (c1 == 0xFFFFu && c0 != 0xFFFEu) last_region = DSP_REGION0;
        else last_region = (c0 > c1) ? DSP_REGION0 : DSP_REGION1;
    } else {
        uint16_t s0 = dsp_r16(DSP_REGION0 + DSP_OFF_DSP_CONFIG);
        uint16_t s1 = dsp_r16(DSP_REGION1 + DSP_OFF_DSP_CONFIG);
        if (s0 != s1)
            last_region = ((int16_t)(s0 - s1) > 0) ? DSP_REGION0 : DSP_REGION1;
    }
    return last_region;
}
static void dsp_audio_frame(void) {
    uint32_t read = dsp_current_region();
    uint32_t write = (read == DSP_REGION0) ? DSP_REGION1 : DSP_REGION0;
    int32_t acc_l[DSP_FRAME_SAMPLES], acc_r[DSP_FRAME_SAMPLES];
    uint32_t dcfg = read + DSP_OFF_APP_DSP_CONFIG;
    uint32_t ddirty = dsp_r32(dcfg);
    unsigned i, n;
    if (ddirty) {
        if (ddirty & (1u << 16)) dsp_master_volume = dsp_rfloat(dcfg + 4u);
        if (ddirty & (1u << 24)) dsp_aux_volume[0] = dsp_rfloat(dcfg + 8u);
        if (ddirty & (1u << 25)) dsp_aux_volume[1] = dsp_rfloat(dcfg + 12u);
        /* Per the local Azahar header ("The DSP clears these each audio
           frame") the DSP consumes the whole dirty word; the app re-posts
           its per-frame headphones bit (bit 28) after every clear.  The
           region sequence counter lives at +0x2860 and is not touched
           here. */
        dsp_w32(dcfg, 0);
    }
    memset(acc_l, 0, sizeof(acc_l));
    memset(acc_r, 0, sizeof(acc_r));
    for (i = 0; i < DSP_NUM_SOURCES; ++i) {
        DspSourceState *s = &dsp_src[i];
        float gl, gr;
        /* Parse BOTH regions every frame.  The middleware alternates regions
           and posts the embedded buffer (dirty bit 30) into whichever region
           it produces into; the host-timed tick can lag a produced frame, so
           reading only the "current" region risks skipping the one frame the
           buffer is dirty.  Scanning both (each parse consumes its dirty
           word, line dsp_w32(cfg,0)) guarantees the buffer is caught once
           regardless of region-pick timing. */
        dsp_parse_source_config(i, DSP_REGION0 + DSP_OFF_SRC_CONFIG + i * 192u);
        dsp_parse_source_config(i, DSP_REGION1 + DSP_OFF_SRC_CONFIG + i * 192u);
        if (s->enabled) {
            gl = dsp_master_volume * (s->gain[0][0] + s->gain[0][2]) +
                 dsp_aux_volume[0] * (s->gain[1][0] + s->gain[1][2]) +
                 dsp_aux_volume[1] * (s->gain[2][0] + s->gain[2][2]);
            gr = dsp_master_volume * (s->gain[0][1] + s->gain[0][3]) +
                 dsp_aux_volume[0] * (s->gain[1][1] + s->gain[1][3]) +
                 dsp_aux_volume[1] * (s->gain[2][1] + s->gain[2][3]);
            for (n = 0; n < DSP_FRAME_SAMPLES; ++n) {
                float p;
                uint32_t i0;
                int32_t vl, vr;
                while (!s->sample_count && s->enabled)
                    if (!dsp_dequeue(i, read)) {
                        s->enabled = 0;
                        s->buffer_update = 1;
                        s->last_buffer_id = s->current_buffer_id;
                        s->current_buffer_id = 0;
                        dsp_sources_finished++;
                    }
                if (!s->enabled) break;
                p = (float)s->pos + s->frac;
                i0 = (uint32_t)p;
                if (i0 + 1u < s->sample_count) {
                    float f = p - (float)i0;
                    float sl0 = dsp_fetch(s, i0, 0), sl1 = dsp_fetch(s, i0 + 1u, 0);
                    vl = (int32_t)(sl0 + (sl1 - sl0) * f);
                    if (s->channels > 1u) {
                        float sr0 = dsp_fetch(s, i0, 1), sr1 = dsp_fetch(s, i0 + 1u, 1);
                        vr = (int32_t)(sr0 + (sr1 - sr0) * f);
                    } else vr = vl;
                } else {
                    vl = dsp_fetch(s, i0, 0);
                    vr = (s->channels > 1u) ? dsp_fetch(s, i0, 1) : vl;
                }
                acc_l[n] += (int32_t)((double)vl * (double)gl);
                acc_r[n] += (int32_t)((double)vr * (double)gr);
                s->frac += s->rate;
                while (s->frac >= 1.0f) {
                    s->frac -= 1.0f;
                    s->pos++;
                    if (s->pos >= s->sample_count) {
                        if (s->looping) {
                            s->pos = 0;
                        } else {
                            dsp_free_buffer(s);
                            if (!dsp_dequeue(i, read)) {
                                s->enabled = 0;
                                s->buffer_update = 1;
                                s->last_buffer_id = s->current_buffer_id;
                                s->current_buffer_id = 0;
                                dsp_sources_finished++;
                            }
                            break;
                        }
                    }
                }
            }
        }
        { /* Source Status in the region the application reads next */
            uint32_t st = write + DSP_OFF_SRC_STATUS + i * 12u;
            ram[st] = s->enabled ? 1u : 0u;
            ram[st + 1u] = s->buffer_update ? 1u : 0u;
            s->buffer_update = 0;
            dsp_w16(st + 2u, s->sync_count);
            dsp_w32dsp(st + 4u, s->pos);
            dsp_w16(st + 8u, s->current_buffer_id);
            dsp_w16(st + 10u, s->last_buffer_id);
        }
    }
    { /* Final Mix Samples (what plays out of the speakers) */
        uint32_t out = write + DSP_OFF_FINAL;
        int16_t frame[DSP_FRAME_SAMPLES * 2u];
        for (n = 0; n < DSP_FRAME_SAMPLES; ++n) {
            int32_t l = acc_l[n], r = acc_r[n];
            if (l > 32767) l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767) r = 32767;
            if (r < -32768) r = -32768;
            dsp_w16(out + n * 4u, (uint16_t)(int16_t)l);
            dsp_w16(out + n * 4u + 2u, (uint16_t)(int16_t)r);
            frame[n * 2u] = (int16_t)l;
            frame[n * 2u + 1u] = (int16_t)r;
        }
        dsp_audio_out_push(frame);
    }
    dsp_w16(write + DSP_OFF_STATUS, 0);        /* dropped frames = 0 */
    dsp_w16(write + DSP_OFF_STATUS + 2u, 0);
}
static void dsp_audio_tick(void) {
    uint64_t now = host_monotonic_ns();
    unsigned caught = 0;
    if (!dsp_audio_on) return;
    if (!dsp_next_frame_ns) dsp_next_frame_ns = now;
    while ((int64_t)(now - dsp_next_frame_ns) >= 0) {
        dsp_audio_frame();
        dsp_next_frame_ns += DSP_FRAME_NS;
        if (++caught >= 8u) { /* fell far behind: resync instead of spiralling */
            dsp_next_frame_ns = now + DSP_FRAME_NS;
            /* PERF-02: an audible underrun.  The backlog past eight frames is
               dropped, so the sink never gets those samples.  A main context
               that busy-polled instead of sleeping starved this tick. */
            ++perf_dsp_resyncs;
            break;
        }
    }
    if (!caught) return;
    ++perf_dsp_ticks;
    /* The audio-pipe interrupt fires once per audio frame and is what keeps
       the guest's audio worker running. */
    if (dsp_pipe_interrupt_event && dsp_pipe_interrupt_event < next_event_handle &&
        event_live[dsp_pipe_interrupt_event]) {
        event_signaled[dsp_pipe_interrupt_event] = 1u;
        guest_notify_handle(dsp_pipe_interrupt_event);
    }
    if (getenv("DQ8_TRACE_AUDIO") && (dsp_frames_out % (DSP_RATE / 4u)) == 0u)
        fprintf(stderr, "dsp audio: frames=%llu started=%llu finished=%llu on=%d\n",
                (unsigned long long)dsp_frames_out, (unsigned long long)dsp_sources_started,
                (unsigned long long)dsp_sources_finished, dsp_audio_on);
}
#endif
/* IDLE THROTTLE hooks: defined in the idle block below (before the HID
   marker); native_audio.c (spliced at this marker) stretches its DSP
   quantum cadence while the throttle is armed. */
static uint64_t idle_stretch_wait(uint64_t ns);
static int idle_throttle_armed(void);
/* NATIVE_AUDIO_RUNTIME */
static uint32_t pica_regs[0x300];
/* VS float uniforms: 96 vec4 (Azahar Uniforms::f). Queue packs f24×4 in 3 words
   or f32×4 in 4 words (PackedAttribute). */
static float vs_uniform_f[96][4];
static uint32_t vs_uniform_q[4];
static unsigned vs_uniform_qi;
static float vs_default_attrib[16][4];
static unsigned vs_default_index, vs_default_qi;
static uint32_t vs_default_q[3];
/* Vertex shader program + swizzle tables (word arrays). */
static uint32_t vs_program[4096];
static unsigned vs_program_size;
static uint32_t vs_swizzle[4096];
static unsigned vs_swizzle_size;
static uint32_t vs_program_off, vs_swizzle_off;
static uint64_t pica_draw_count;
static uint64_t pica_finalize_count;
static unsigned gsp_pica_dump_index;
static uint32_t mem8(uint32_t);
static uint32_t mem32(uint32_t);
static void put8(uint32_t, uint32_t);
static uint32_t guest_memory_address(uint32_t);
static int guest_memory_range(uint32_t, uint64_t, uint32_t *);
static void gsp_enqueue_interrupt(uint8_t event);
static void gsp_write_screen_ppms(void);
static void gsp_scanout_capture(void);
/* Scan-out slot state (Azahar GPU::SetBufferSwap model): which buffer the LCD
   shows per screen. Defined with gsp_scanout_capture below. */
static void dlog_note_draw(void);   /* t_fa78ea1f DARKLOG per-RT tally */
static uint32_t gsp_fb_addr[2][2];   /* [screen][slot] */
static uint32_t gsp_fb_fmt[2], gsp_fb_stride[2];
static uint32_t gsp_fb_shown[2];
static int gsp_fb_valid[2];
/* Old 3DS LINEAR memory at 0x14000000 is a CPU alias of FCRAM at
   0x20000000. DQ8 asks GetProcessInfo for this exact 0x0C000000 delta,
   then passes physical FCRAM addresses to GSP/GPU services. Keep both views
   coherent in the flat guest RAM image.

   FCRAM offsets < 128MB alias to LINEAR 0x14000000. Offsets above that
   (N3DS 256MB FCRAM) only appear at NEW_LINEAR 0x30000000 + (off-0x08000000). */
static uint32_t guest_memory_address(uint32_t a) {
    /* VRAM PAddr 0x18000000.. → VAddr 0x1F000000.. (azahar memory.h). */
    if (a >= 0x18000000u && a < 0x18600000u)
        return a - 0x18000000u + 0x1F000000u;
    if (a >= 0x20000000u && a < 0x30000000u) {
        uint32_t off = a - 0x20000000u;
        /* LINEAR 0x14000000+off and NEW_LINEAR 0x30000000+off both alias FCRAM. */
        return (off < 0x08000000u) ? (0x14000000u + off) : (0x30000000u + off);
    }
    return a;
}
static int guest_memory_range(uint32_t a, uint64_t size, uint32_t *mapped) {
    a = guest_memory_address(a);
    if (a > GUEST_RAM_SIZE || size > (uint64_t)GUEST_RAM_SIZE - a) return 0;
    *mapped = a;
    return 1;
}
/* Guest GPU commands load attributes, execute the uploaded PICA vertex program,
   and rasterize its outputs into the color buffer used by display transfers. */
/* Guest PICA framebuffer state (Azahar FramebufferConfig). */
static uint32_t pica_rt_addr = 0x1F1F0800u;
static uint32_t pica_rt_w = 480u, pica_rt_h = 400u;
static uint32_t pica_rt_format = 0u; /* ColorFormat RGBA8 (0 is valid) */

/* PICA ColorFormat: 0 RGBA8, 1 RGB8, 2 RGB5A1, 3 RGB565, 4 RGBA4.
   GPU packs components in reverse byte order (A,B,G,R for RGBA8). */
static unsigned pica_color_bpp(uint32_t fmt) {
    switch (fmt & 7u) {
    case 0u: return 4u;
    case 1u: return 3u;
    case 2u: case 3u: case 4u: return 2u;
    default: return 4u;
    }
}

/* Layer-order forensics (task t_bd54cc6d).  DQ8_TRACE_LAYER=START[,DUR] logs
   every command list, memory fill and display transfer with monotonic seconds
   since process start, so the pass that lands last in a frame can be named.
   DQ8_TRACE_RECT=x0,y0,x1,y1 logs each fragment write inside that rectangle
   (draw index, list, source colour, stored colour) until the cap is hit. */
/* Frame meter for DQ8_TRACE_TIME (t_399eab58): incremented on every PDC0
   vblank scan-out evaluation (gsp_enqueue_interrupt), reported per window
   by pqt_account (pica_vs.c). */
static unsigned long long pqt_frames;
static double dq8_mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static double dq8_t0_s(void) { static double t0; if (!t0) t0 = dq8_mono_s(); return t0; }
static double dq8_layer_from = 0.0, dq8_layer_dur = 1e9;
static int dq8_layer_trace(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DQ8_TRACE_LAYER");
        on = (e && e[0] && e[0] != '0');
        if (on) {
            dq8_layer_from = 0.0; dq8_layer_dur = 1e9;
            if (strchr(e, ',')) sscanf(e, "%lf,%lf", &dq8_layer_from, &dq8_layer_dur);
        }
    }
    if (!on) return 0;
    double t = dq8_mono_s() - dq8_t0_s();
    return t >= dq8_layer_from && t <= dq8_layer_from + dq8_layer_dur;
}
static double dq8_layer_t(void) { return dq8_mono_s() - dq8_t0_s(); }

/* t_275eb521: cached env gates for the hot paths.  getenv() is a linear walk
   of `environ`, and these knobs were being re-read from code that runs per
   PICA register write (the command-list walker), per draw, per indirect
   branch, per GSP fill/transfer and inside the main-context wait spin.  None
   of them can change while the process runs, so each is resolved once per
   process -- the same pattern as tempo_get()/dq8_layer_trace().
   dq8_env_on() keeps every call site's original test exactly ("is the
   variable present at all", which also enables it for an empty or "0"
   value).  Knobs whose VALUE matters are parsed once by their own helpers
   instead (pvs_dec_sync, pgl_setup, ...). */
static int dq8_env_trace_gpu = -1;      /* DQ8_TRACE_GPU */
static int dq8_env_trace_texcfg = -1;   /* DQ8_TRACE_TEXCFG */
static int dq8_env_trace_layer = -1;    /* DQ8_TRACE_LAYER, raw presence */
static int dq8_env_trace_gsp_fb = -1;   /* DQ8_TRACE_GSP_FB */
static int dq8_env_trace_branch = -1;   /* DQ8_TRACE_BRANCH */
static int dq8_env_strict_null = -1;    /* DQ8_STRICT_NULL_CALLBACK */
static int dq8_env_trace_wait = -1;     /* DQ8_TRACE_WAIT */
static int dq8_env_trace_ipc = -1;      /* DQ8_TRACE_IPC */
static int dq8_env_trace_fmtpath = -1;  /* DQ8_TRACE_FORMAT_PATH */
static int dq8_env_trace_arbiter = -1;  /* DQ8_TRACE_ARBITER */
static uint32_t dq8_env_arbiter_addr;    /* DQ8_TRACE_ARBITER_ADDRESS, parsed */
static int dq8_env_arbiter_addr_init;
static const char *dq8_env_scan_msg;    /* DQ8_SCAN_MSG */
static int dq8_env_scan_msg_init;
static int dq8_env_on(const char *name, int *gate) {
    if (*gate < 0) *gate = getenv(name) != NULL;
    return *gate;
}
static unsigned long long pica_list_no;
/* Screen-space union of everything rasterized in the current command list. */
static uint32_t pica_bb_x0 = 0xFFFFFFFFu, pica_bb_y0 = 0xFFFFFFFFu;
static uint32_t pica_bb_x1 = 0, pica_bb_y1 = 0;
static void pica_bb_add(int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if ((uint32_t)x < pica_bb_x0) pica_bb_x0 = (uint32_t)x;
    if ((uint32_t)y < pica_bb_y0) pica_bb_y0 = (uint32_t)y;
    if ((uint32_t)x > pica_bb_x1) pica_bb_x1 = (uint32_t)x;
    if ((uint32_t)y > pica_bb_y1) pica_bb_y1 = (uint32_t)y;
}
static void pica_bb_reset(void) {
    pica_bb_x0 = pica_bb_y0 = 0xFFFFFFFFu; pica_bb_x1 = pica_bb_y1 = 0;
}
/* Per-draw extent (reset for every draw trigger) + one trace line per draw,
   so the pass order INSIDE a command list can be read off the log. */
static uint32_t pica_db_x0 = 0xFFFFFFFFu, pica_db_y0 = 0xFFFFFFFFu;
static uint32_t pica_db_x1 = 0, pica_db_y1 = 0;
static void pica_db_reset(void) {
    pica_db_x0 = pica_db_y0 = 0xFFFFFFFFu; pica_db_x1 = pica_db_y1 = 0;
}
static void pica_db_add(int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if ((uint32_t)x < pica_db_x0) pica_db_x0 = (uint32_t)x;
    if ((uint32_t)y < pica_db_y0) pica_db_y0 = (uint32_t)y;
    if ((uint32_t)x > pica_db_x1) pica_db_x1 = (uint32_t)x;
    if ((uint32_t)y > pica_db_y1) pica_db_y1 = (uint32_t)y;
}
/* Guest vertex colour alpha vs the alpha our TEV pipeline produced for the
   current draw: a mismatch here is a blend/alpha bug, a match with a visible
   menu means the guest really drew it. */
static float pica_draw_prim_a = 1.0f, pica_draw_tev_a = 1.0f;
/* DQ8_TRACE_PX probe helpers (defined with the pica_vs runtime below). */
static void pica_px_report(void);
static int pica_px_find(int x, int y);
/* t_0b95f1a7 black-3D forensics globals (defined in pica_vs.c, spliced later).
   t_968a1fa9: these are PER-THREAD (see pica_vs.c) so the parallel vertex pool
   cannot clobber another vertex's failure verdict; the forward declaration
   must carry the same storage class. */
static __thread unsigned pica_vs_fail_reason, pica_vs_fail_detail;
static unsigned pica_draw_log_count, pica_draw_log_tris, pica_draw_log_clip;
static unsigned pica_draw_log_nfdrop;
static unsigned pica_draw_log_arej, pica_draw_log_drej, pica_draw_log_frw;
static float pica_draw_log_wmin, pica_draw_log_wmax;
/* t_41f38df0 cutscene-camera forensics: pica_uni_cam_trace (pica_vs.c) logs
   the camera/model uniforms of the draws DQ8_TRACE_UNIDRAW asks for and calls
   pica_dump_current_cmdlist to capture the command list of the CLIPPED
   DQ8_DUMP_PICA_COUNT draw (its float-uniform uploads decode offline). */
static uint32_t pica_cur_list_addr, pica_cur_list_size;
/* t_41f38df0: the frame stack around pica_cur_list_*.  The clipped draw lives
   in a NESTED 320-byte fragment (bw-cam5: cmdbin-0001-20F64000-320.bin held the
   four draw triggers but no 0x2C0 uniform upload) — the camera floats are in an
   enclosing list, so the dump must capture every level, not just the innermost. */
static uint32_t pica_list_frame_addr[8], pica_list_frame_size[8];
static unsigned pica_list_frame_n;
static void pica_uni_cam_trace(unsigned count);
/* t_41f38df0: capture the command list that is being processed RIGHT NOW
   (the one containing the draw that armed the probe).  Done at arm time,
   not at end-of-list: a 0x230 trigger write can start a nested list before
   the outer one finishes, and the old end-of-wrapper dump fired on that
   nested list instead (all 36 bw-cam1 dumps were 32-byte, draws=0). */
static void pica_dump_current_cmdlist(unsigned count) {
    static unsigned dump_done;
    const char *dir = getenv("DQ8_DUMP_PICA_COUNT_DIR");
    if (!dir || !*dir || !pica_cur_list_addr || !pica_cur_list_size) return;
    if (dump_done) return;              /* one-shot: first dead hit only */
    /* only the CLIPPED instance of the probed draw (the cutscene one); the
       world-state instance renders fine and would win the race otherwise */
    if (pica_draw_log_tris || pica_draw_log_clip < (int)count) return;
    /* bw-cam4 never dumped: fopen() into a not-yet-created directory failed
       silently.  Create the leaf (mkdir(2) is a no-op when it exists). */
    mkdir(dir, 0755);
    char path[1024];
    /* Every enclosing level, outermost first: the innermost frame is only the
       draw-trigger fragment (bw-cam5: 320 bytes, four 0x228 writes, no uniform
       upload) — the camera floats live further out. */
    unsigned nframes = pica_list_frame_n;
    for (unsigned d = 0; d <= nframes; ++d) {
        uint32_t a = (d < nframes) ? pica_list_frame_addr[d] : pica_cur_list_addr;
        uint32_t s = (d < nframes) ? pica_list_frame_size[d] : pica_cur_list_size;
        if (!a || !s) continue;
        uint32_t mapped;
        if (!guest_memory_range(a, s, &mapped)) continue;
        snprintf(path, sizeof path, "%s/cmdbin-%04u-d%u-%08X-%u.bin", dir, 1u,
                 d, a, s);
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "CMDBIN FAILED %s: %s count=%u\n", path, strerror(errno), count);
            continue;
        }
        size_t wrote = fwrite(ram + mapped, 1u, s, f);
        fclose(f);
        fprintf(stderr, "CMDBIN dumped %zu/%u bytes -> %s list=%llu depth=%u count=%u\n",
                wrote, s, path, (unsigned long long)pica_list_no, d, count);
    }
    dump_done = 1;
}
static void pica_log_draw(int failed) {
    pica_uni_cam_trace(pica_draw_log_count);
    pica_px_report();
    if (!dq8_layer_trace()) return;
    fprintf(stderr, "DRAW t=%.2f list=%llu draw=%llu count=%u tris=%u rsn=%u det=%08X gs=%X clip=%u nfdrop=%u arej=%u drej=%u frw=%u wmin=%.4g wmax=%.4g bbox=%u,%u..%u,%u blend=%08X/%08X mask=%08X atest=%08X rt=%08X prim_a=%.3f tev_a=%.3f%s\n",
            dq8_layer_t(), pica_list_no, (unsigned long long)pica_draw_count,
            pica_draw_log_count, pica_draw_log_tris,
            pica_vs_fail_reason, pica_vs_fail_detail, pica_regs[0x229] & 0x80000003u,
            pica_draw_log_clip, pica_draw_log_nfdrop, pica_draw_log_arej, pica_draw_log_drej, pica_draw_log_frw, pica_draw_log_wmin, pica_draw_log_wmax,
            pica_db_x0, pica_db_y0, pica_db_x1, pica_db_y1,
            pica_regs[0x100], pica_regs[0x101], pica_regs[0x107], pica_regs[0x104],
            pica_rt_addr, pica_draw_prim_a, pica_draw_tev_a, failed ? " FAILED" : "");
}
/* DQ8_TRACE_RECT: rect (inclusive, RT pixel coordinates) + optional :cap */
static int pica_rect_init = -1, pica_rect_rx0, pica_rect_ry0, pica_rect_rx1, pica_rect_ry1;
static unsigned pica_rect_cap, pica_rect_hits;
/* True when the DQ8_TRACE_RECT trace is armed.  The fragment-write paths use
   this to skip quantizing the source colour to bytes, which is only ever
   consumed by this trace (blend fast path, t_568f6976). */
static int pica_rect_src_needed(void) {
    if (pica_rect_init < 0) {
        const char *e = getenv("DQ8_TRACE_RECT");
        pica_rect_init = (e && e[0]) ? 1 : 0;
        if (pica_rect_init) {
            int n = sscanf(e, "%d,%d,%d,%d:%u", &pica_rect_rx0, &pica_rect_ry0,
                           &pica_rect_rx1, &pica_rect_ry1, &pica_rect_cap);
            if (n < 4) pica_rect_init = 0;
            if (!pica_rect_cap) pica_rect_cap = 4000u;
        }
    }
    return pica_rect_init;
}
static int pica_rect_log(int x, int y, const uint8_t src[4], const uint8_t out[4]) {
    if (!pica_rect_src_needed() || pica_rect_hits >= pica_rect_cap) return 0;
    if (!dq8_layer_trace()) return 0;
    if (x < pica_rect_rx0 || x > pica_rect_rx1 || y < pica_rect_ry0 || y > pica_rect_ry1) return 0;
    ++pica_rect_hits;
    fprintf(stderr, "RECT t=%.2f list=%llu draw=%llu px=%d,%d src=%u/%u/%u/%u out=%u/%u/%u/%u rt=%08X\n",
            dq8_layer_t(), pica_list_no, (unsigned long long)pica_draw_count, x, y,
            src[0], src[1], src[2], src[3], out[0], out[1], out[2], out[3], pica_rt_addr);
    return 1;
}

static void pica_bind_framebuffer(void) {
    /* Color addr @0x11D bits 0-27 * 8. Dim encoding not yet matched; keep
       observed RT size when w/h parse fails. ColorFormat 0=RGBA8 is valid. */
    uint32_t addr_field = pica_regs[0x11D] & 0x0FFFFFFFu;
    uint32_t fmt = (pica_regs[0x117] >> 16) & 7u;
    uint32_t w = pica_regs[0x11E] & 0x7FFu;
    uint32_t h = ((pica_regs[0x11E] >> 12) & 0x3FFu) + 1u;
    if (!addr_field) return;
    uint32_t paddr = addr_field * 8u;
    uint32_t va = guest_memory_address(paddr);
    pica_rt_addr = va;
    if (w >= 32u && h >= 32u && w <= 1024u && h <= 1024u) {
        pica_rt_w = w;
        pica_rt_h = h;
    }
    if (fmt <= 4u) pica_rt_format = fmt;
    if (dq8_layer_trace()) {
        static uint32_t la, lw, lh, lf;
        if (la != pica_rt_addr || lw != pica_rt_w || lh != pica_rt_h || lf != pica_rt_format) {
            fprintf(stderr, "LAYER t=%.2f bind rt=%08X %ux%u fmt=%u (was %08X %ux%u fmt=%u) draw=%llu\n",
                    dq8_layer_t(), pica_rt_addr, pica_rt_w, pica_rt_h, pica_rt_format,
                    la, lw, lh, lf, (unsigned long long)pica_draw_count);
            la = pica_rt_addr; lw = pica_rt_w; lh = pica_rt_h; lf = pica_rt_format;
        }
    }
    if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu) && pica_draw_count <= 2u)
        fprintf(stderr, "PICA fb paddr=%08X va=%08X dim=%08X rt=%08X %ux%u fmt=%u\n",
                paddr, va, pica_regs[0x11E], pica_rt_addr, pica_rt_w, pica_rt_h, pica_rt_format);
}

/* Depth buffer + depth test (Azahar: FramebufferRegs::depth_buffer_address,
   RasterizerRegs::viewport_depth_range/near_plane, DoDepthStencilTest).
   The guest clears this buffer itself with GSP memory-fill (observed:
   1F419400..1F4E1400 value=00FFFFFF = D24 far plane) and every UI draw is
   submitted with depth test enabled + depth write (depth_color_mask 0x107
   bit0/bit12, func LEQUAL).  Without it a later-drawn pass always lands on
   top of an earlier one, which is exactly the naming-screen-under-menu bug. */
static uint32_t pica_depth_addr;                 /* guest VA of depth buffer */
static unsigned pica_depth_bpp = 4u;             /* bytes per depth pixel */
static unsigned pica_depth_bits = 24u;
static unsigned pica_depth_allow_write = 1u;
static int pica_depth_bound;
static int pica_depth_log_done;
static float pica_f24_to_f32(uint32_t raw);      /* defined in pica_vs.c */
static void pica_bind_depth(void) {
    uint32_t addr_field = pica_regs[0x11C] & 0x0FFFFFFFu;
    uint32_t fmt = pica_regs[0x117] & 3u;        /* 0=D16 2=D24 3=D24S8 */
    pica_depth_allow_write = pica_regs[0x116] & 3u;
    if (!addr_field) { pica_depth_addr = 0; pica_depth_bound = 0; return; }
    pica_depth_addr = guest_memory_address(addr_field * 8u);
    /* Stride per DEPTH pixel. D24 occupies a full 32-bit Morton texel on the
       surface/GPU side (azahar rasterizer_cache/pixel_format.h FORMAT_MAP:
       {SurfaceType::Depth, 24, 4}, "Modern GPUs require 4 byte alignment for
       D24") and the guest allocates+fills exactly w*h*4 bytes with 32-bit
       words (FF FF FF 00). Reading 3-byte windows over that pattern yields
       parity-dependent stale refs (4 Morton classes by x/my parity) which
       failed LEQUAL on alternating memory rows -> the 1px column striping on
       the save-menu screens. With 4-byte pixels every window reads FF,FF,FF =
       far plane until we overwrite it, so the fill and the test agree. */
    pica_depth_bpp = (fmt == 0u) ? 2u : 4u;   /* D16=2; D24/D24S8=4 */
    pica_depth_bits = (fmt == 0u) ? 16u : 24u;
    pica_depth_bound = 1;
    if (!pica_depth_log_done) {
        pica_depth_log_done = 1;
        fprintf(stderr, "DEPTH first bind addr_field=%08X va=%08X fmt=%u bpp=%u allow_w=%u "
                        "range=%08X(%.4f) near=%08X(%.4f) depthmap(Z=%u) rt=%08X %ux%u\n",
                addr_field, pica_depth_addr, fmt, pica_depth_bpp, pica_depth_allow_write,
                pica_regs[0x4D], pica_f24_to_f32(pica_regs[0x4D]),
                pica_regs[0x4E], pica_f24_to_f32(pica_regs[0x4E]),
                pica_regs[0x6D] & 1u, pica_rt_addr, pica_rt_w, pica_rt_h);
    }
}
/* Morton-tiled offset of a depth pixel, same layout as the color buffer. */
static uint32_t pica_depth_off(int x, int y) {
    static const uint32_t xlut[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
    static const uint32_t ylut[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
    uint32_t px = (uint32_t)x, py = (uint32_t)y;
    uint32_t i = xlut[px & 7u] + ylut[py & 7u];
    uint32_t row = (py >> 3) * (pica_rt_w >> 3) * 64u + (px & ~7u) * 8u;
    return (row + i) * pica_depth_bpp;
}
static int pica_depth_range_ok(void) {
    uint32_t bytes = (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp;
    uint32_t mapped;
    return pica_depth_addr && guest_memory_range(pica_depth_addr, bytes, &mapped);
}
/* Returns 1 when the fragment passes the depth test; writes the new depth
   value when depth writing is enabled. */
static int pica_depth_test_write(int x, int y, float depth) {
    uint32_t mask = pica_regs[0x107];
    int test_enable = mask & 1u;
    int write_enable = (mask & (1u << 12)) && pica_depth_allow_write;
    if (!pica_depth_bound || !pica_depth_range_ok())
        return 1;
    uint32_t mapped;
    guest_memory_range(pica_depth_addr, (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp, &mapped);
    int my = (int)pica_rt_h - 1 - y;             /* raster bottom-up -> row top-down */
    if (x < 0 || my < 0 || (uint32_t)x >= pica_rt_w || (uint32_t)my >= pica_rt_h) return 1;
    uint32_t off = pica_depth_off(x, my);
    uint8_t *p = ram + mapped + off;
    float d = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    uint32_t zmax = (pica_depth_bits == 16u) ? 0xFFFFu : 0xFFFFFFu;
    uint32_t z = (uint32_t)(d * (float)zmax);     /* Azahar truncates */
    if (test_enable) {
        uint32_t ref;
        if (pica_depth_bpp == 2u) { uint16_t v; memcpy(&v, p, 2); ref = v; }
        else ref = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        unsigned func = (mask >> 4) & 7u;         /* CompareFunc */
        int pass;
        switch (func) {
        case 0u: pass = 0; break;                 /* Never */
        case 1u: pass = 1; break;                 /* Always */
        case 2u: pass = z == ref; break;          /* Equal */
        case 3u: pass = z != ref; break;          /* NotEqual */
        case 4u: pass = z < ref; break;           /* Less */
        case 5u: pass = z <= ref; break;          /* LessThanOrEqual */
        case 6u: pass = z > ref; break;           /* Greater */
        default: pass = z >= ref; break;          /* GreaterThanOrEqual */
        }
        if (!pass) {
            int pi = pica_px_find(x, y);
            if (pi >= 0)
                fprintf(stderr, "PXF t=%.2f draw=%llu px=%d,%d z=%06X ref=%06X f=%u\n",
                        dq8_layer_t(), (unsigned long long)pica_draw_count, x, y,
                        z, ref, func);
            return 0;
        }
    }
    {   /* DQ8_ZWRITE0_TRACE (diagnosis only, off by default): who stores a
           zero depth value into guest memory (software-rasterizer path). */
        static int t0, tinit;
        if (!tinit) {
            const char *e = getenv("DQ8_ZWRITE0_TRACE");
            t0 = e && e[0] && e[0] != '0';
            tinit = 1;
        }
        if (t0 && write_enable && z == 0u)
            fprintf(stderr, "ZW0 draw=%llu x=%d row=%d test=%d func=%u\n",
                    (unsigned long long)pica_draw_count, x, my, test_enable,
                    (unsigned)((mask >> 4) & 7u));
    }
    if (write_enable) {
        p[0] = (uint8_t)(z & 0xFFu);
        p[1] = (uint8_t)((z >> 8) & 0xFFu);
        if (pica_depth_bpp >= 3u) p[2] = (uint8_t)((z >> 16) & 0xFFu);
    }
    return 1;
}
/* Depth value for a fragment: Z-buffering uses z/w*scale+offset, W-buffering
   multiplies by the perspective-correct w (Azahar sw_rasterizer.cpp:342-356). */
static float pica_fragment_depth(float zw, float iw_interp, float scale, float offset) {
    float depth = zw * scale + offset;
    if (!(pica_regs[0x6D] & 1u) && iw_interp > 0.0f)
        depth /= iw_interp;                       /* * perspective-correct w */
    return depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
}

static uint32_t pica_rt_off(int x, int y) {
    /* Tiled 8x8 Morton color buffer (sw_framebuffer / GetMortonOffset). */
    static const uint32_t xlut[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
    static const uint32_t ylut[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
    uint32_t px = (uint32_t)x, py = (uint32_t)y;
    uint32_t coarse_x = px & ~7u;
    uint32_t i = xlut[px & 7u] + ylut[py & 7u];
    uint32_t row = (py >> 3) * (pica_rt_w >> 3) * 64u + coarse_x * 8u;
    return (row + i) * pica_color_bpp(pica_rt_format);
}

static void pica_put_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t mapped;
    if (x < 0 || y < 0 || (uint32_t)x >= pica_rt_w || (uint32_t)y >= pica_rt_h) return;
    if (!guest_memory_range(pica_rt_addr, pica_rt_w * pica_rt_h * 4u, &mapped)) return;
    uint8_t *p = ram + mapped + pica_rt_off(x, y);
    uint32_t fmt = pica_rt_format;
    if (fmt == 0u) { /* RGBA8 stored A,B,G,R */
        p[0] = a; p[1] = b; p[2] = g; p[3] = r;
    } else if (fmt == 1u) { /* RGB8 stored B,G,R */
        p[0] = b; p[1] = g; p[2] = r;
    } else if (fmt == 3u) {
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        memcpy(p, &v, 2);
    } else if (fmt == 2u) {
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
        memcpy(p, &v, 2);
    } else {
        uint16_t v = (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4));
        memcpy(p, &v, 2);
    }
}

/* Read a PICA color pixel (tiled) into logical RGBA. */
static int pica_get_pixel(uint32_t base, uint32_t w, uint32_t h, uint32_t fmt,
                          uint32_t x, uint32_t y,
                          uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    if (x >= w || y >= h) return 0;
    uint32_t mapped;
    if (!guest_memory_range(base, w * h * 4u, &mapped)) return 0;
    static const uint32_t xlut[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
    static const uint32_t ylut[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
    uint32_t bpp = pica_color_bpp(fmt);
    uint32_t off = (((y >> 3) * (w >> 3) + (x >> 3)) * 64u + xlut[x & 7u] + ylut[y & 7u]) * bpp;
    if ((uint64_t)mapped + off + bpp > GUEST_RAM_SIZE) return 0;
    const uint8_t *p = ram + mapped + off;
    *r = *g = *b = 255; *a = 255;
    if (fmt == 0u) { *a = p[0]; *b = p[1]; *g = p[2]; *r = p[3]; }
    else if (fmt == 1u) { *b = p[0]; *g = p[1]; *r = p[2]; }
    else if (fmt == 3u) {
        uint16_t v; memcpy(&v, p, 2);
        *r = (uint8_t)(((v >> 11) & 31) * 255 / 31);
        *g = (uint8_t)(((v >> 5) & 63) * 255 / 63);
        *b = (uint8_t)((v & 31) * 255 / 31);
    } else return 0;
    return 1;
}

/* Find a BFLIM in guest RAM and sample IA8/ETC1/RGB565. Returns 0 if none. */
static uint32_t bflim_hdr_cache;
static int pica_sample_bflim(float u, float v, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    if (!bflim_hdr_cache) {
        unsigned best_score = 0;
        for (uint32_t a = 0x08000000u; a + 40u < 0x18000000u; a += 4u) {
            if (!(ram[a]=='F' && ram[a+1]=='L' && ram[a+2]=='I' && ram[a+3]=='M')) continue;
            unsigned hdr = a;
            if (a >= 40u && ram[a-36]=='i' && ram[a-35]=='m') hdr = a - 40;
            uint16_t w = ram[hdr+28] | (ram[hdr+29]<<8);
            uint16_t h = ram[hdr+30] | (ram[hdr+31]<<8);
            uint8_t fmt = ram[hdr+34];
            unsigned score = 0;
            if (w == 256 && h == 32 && fmt == 5u) score = 100;      /* sqex_logo_000 */
            else if (w == 64 && h == 32 && fmt == 5u) score = 90;   /* sqex_logo_001 */
            else if (w == 512 && h == 256 && fmt == 5u) score = 80; /* upper bg */
            else if (w == 256 && h == 256) score = 70;               /* map */
            if (score > best_score) {
                best_score = score;
                bflim_hdr_cache = hdr;
            }
        }
    }
    if (!bflim_hdr_cache) return 0;
    unsigned hdr = bflim_hdr_cache;
    uint16_t w = ram[hdr+28] | (ram[hdr+29]<<8);
    uint16_t h = ram[hdr+30] | (ram[hdr+31]<<8);
    uint8_t fmt = ram[hdr+34];
    uint32_t raw = ram[hdr+36] | (ram[hdr+37]<<8) | (ram[hdr+38]<<16) | (ram[hdr+39]<<24);
    if (!w || !h || raw > 4u * 1024u * 1024u) return 0;
    uint32_t px = (uint32_t)(u * (float)(w ? w - 1 : 0));
    uint32_t py = (uint32_t)(v * (float)(h ? h - 1 : 0));
    if (px >= w) px = w - 1;
    if (py >= h) py = h - 1;
    const uint8_t *pix = ram + hdr - raw;
    {
        static int logged;
        if (!logged++) {
            fprintf(stderr, "BFLIM use @%08X %ux%u fmt=%u raw=%u uv=%.2f,%.2f\n",
                    hdr, w, h, fmt, raw, u, v);
        }
    }
    /* fmt 5 = IA8 (PICA), 3 = RGB565, 7 = I8, 8 = A8, 10 = ETC1 */
    if (fmt == 5u) {
        /* BFLIM v7.2.0 RGB565, orientation 4 (tools/decode_sqex_logo.py):
           stored surface is (h x w), Morton 8x8 tiles, then ROTATE_90 CCW.
           display(x,y) = stored(y, stored_w-1-x) after rotation. */
        uint32_t stored_w = h, stored_h = w;
        uint32_t sx = (stored_w - 1u) - py;
        uint32_t sy = px;
        uint32_t tile = (sy / 8u) * (stored_w / 8u) + sx / 8u;
        uint32_t mx = sx % 8u, my = sy % 8u;
        uint32_t m = 0;
        for (unsigned bit = 0; bit < 3u; ++bit)
            m |= (((mx >> bit) & 1u) << (2u * bit)) | (((my >> bit) & 1u) << (2u * bit + 1u));
        uint32_t off = 2u * (64u * tile + m);
        if (hdr < raw || (uint64_t)(hdr - raw) + off + 2u > GUEST_RAM_SIZE) return 0;
        uint16_t v16;
        memcpy(&v16, pix + off, 2);
        uint32_t r5 = (v16 >> 11) & 31u, g6 = (v16 >> 5) & 63u, b5 = v16 & 31u;
        *r = (uint8_t)((r5 << 3) | (r5 >> 2));
        *g = (uint8_t)((g6 << 2) | (g6 >> 4));
        *b = (uint8_t)((b5 << 3) | (b5 >> 2));
        *a = 255;
        return 1;
    }
    if (fmt == 3u) {
        uint32_t i = (py * w + px) * 2u;
        uint16_t v16;
        memcpy(&v16, pix + i, 2);
        *r = (uint8_t)(((v16 >> 11) & 31) * 255 / 31);
        *g = (uint8_t)(((v16 >> 5) & 63) * 255 / 63);
        *b = (uint8_t)((v16 & 31) * 255 / 31);
        *a = 255;
        return 1;
    }
    if (fmt == 7u || fmt == 8u) {
        uint32_t i = py * w + px;
        *r = *g = *b = (fmt == 7u) ? pix[i] : 255u;
        *a = (fmt == 8u) ? pix[i] : 255u;
        return 1;
    }
    /* ETC1 fmt 10: decode one 4x4 block */
    if (fmt == 10u) {
        uint32_t bx = px / 4, by = py / 4;
        uint32_t blocks_x = (w + 3) / 4;
        uint32_t tiles_x = (blocks_x + 1) / 2, tiles_y = ((h + 3) / 4 + 1) / 2;
        uint32_t tile_x = bx / 2, tile_y = by / 2;
        uint32_t bi = ((tile_y * tiles_x + tile_x) * 4) + (by % 2) * 2 + (bx % 2);
        uint32_t off = bi * 8;
        if (hdr - raw + off + 8u > GUEST_RAM_SIZE) return 0;
        /* simplified ETC1: use average of block bytes as gray */
        unsigned sum = 0;
        for (unsigned k = 0; k < 8; ++k) sum += pix[off + k];
        *r = *g = *b = (uint8_t)(sum / 8);
        *a = 255;
        return 1;
    }
    return 0;
}
/* Wrap/border per Azahar SwRenderer::GetWrappedTexCoord
   (azahar-master src/video_core/renderer_software/sw_texturing.cpp:16-54). */
static int pica_wrap_coord(uint32_t mode, int val, uint32_t size) {
    switch (mode & 7u) {
    case 0u: /* ClampToEdge */
        if (val < 0) val = 0;
        if (val >= (int)size) val = (int)size - 1;
        return val;
    case 1u: /* ClampToBorder: coord passes; out-of-range handled by caller */
        return val;
    case 4u: /* ClampToEdge2: negative repeats, non-negative clamps */
        if (val < 0) return (int)((uint32_t)val % size);
        if (val >= (int)size) val = (int)size - 1;
        return val;
    case 2u: case 6u: case 7u: /* Repeat / Repeat2 / Repeat3 */
        return (int)((uint32_t)val % size);
    case 5u: /* ClampToBorder2: negative repeats; >=size handled by caller */
        if (val < 0) return (int)((uint32_t)val % size);
        return val;
    default: { /* 3: MirroredRepeat */
        uint32_t coord = (uint32_t)val % (2u * size);
        if (coord >= size) coord = 2u * size - 1u - coord;
        return (int)coord;
    }
    }
}
static int pica_sample_tex0(float u, float v, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    uint32_t addr_field = pica_regs[0x85] & 0x0FFFFFFFu;
    uint32_t w = (pica_regs[0x82] >> 16) & 0x7FFu;
    uint32_t h = pica_regs[0x82] & 0x7FFu;
    uint32_t fmt = pica_regs[0x8E] & 0xFu;
    if (!addr_field || !w || !h) return 0;
    uint32_t paddr = addr_field * 8u;
    uint32_t va = guest_memory_address(paddr);
    uint32_t mapped;
    if (!guest_memory_range(va, 4u, &mapped)) return 0;
    /* Azahar RasterizerSoftware::TextureColor (sw_rasterizer.cpp:530-566):
       s=(s32)(u*width) with NO pre-clamp; wrap modes come from
       GPUREG_TEXUNIT0_PARAM (wrap_t bits 8-10, wrap_s bits 12-14) and the
       border colour from GPUREG_TEXUNIT0_BORDER_COLOR (0x81, r/g/b/a bytes). */
    int s = (int)(u * (float)w);
    int t = (int)(v * (float)h);
    uint32_t wrap_s = (pica_regs[0x83] >> 12) & 7u;
    uint32_t wrap_t = (pica_regs[0x83] >> 8) & 7u;
    int border_s = (wrap_s == 1u) ? (s < 0 || s >= (int)w)
                  : (wrap_s == 5u) ? (s >= (int)w) : 0;
    int border_t = (wrap_t == 1u) ? (t < 0 || t >= (int)h)
                  : (wrap_t == 5u) ? (t >= (int)h) : 0;
    if (border_s || border_t) {
        uint32_t bc = pica_regs[0x81];
        *r = (uint8_t)(bc & 0xFFu);
        *g = (uint8_t)((bc >> 8) & 0xFFu);
        *b = (uint8_t)((bc >> 16) & 0xFFu);
        *a = (uint8_t)((bc >> 24) & 0xFFu);
        return 1;
    }
    s = pica_wrap_coord(wrap_s, s, w);
    /* PICA textures are stored bottom-up: azahar inverts t after wrapping. */
    t = (int)h - 1 - pica_wrap_coord(wrap_t, t, h);
    uint32_t x = (uint32_t)s, y = (uint32_t)t;
    /* PICA texture formats: 0 RGBA8, 1 RGB8, 2 RGB5A1, 3 RGB565, 4 RGBA4,
       5 IA8, 6 RG8, 7 I8, 8 A8, 9 IA4, 10 I4, 11 A4, 12 ETC1, 13 ETC1A4.
       Layout = Morton 8x8 tiles; per-tile size from Azahar CalculateTileSize. */
    uint32_t tile_size;
    switch (fmt) {
    case 0u: tile_size = 4u * 64u; break;                 /* RGBA8 */
    case 1u: tile_size = 3u * 64u; break;                 /* RGB8 */
    case 2u: case 3u: case 4u: case 5u: case 6u: tile_size = 2u * 64u; break;
    case 7u: case 8u: tile_size = 64u; break;             /* I8, A8 */
    case 9u: tile_size = 64u; break;                      /* IA4: 1 byte/texel */
    case 10u: case 11u: tile_size = 32u; break;           /* I4, A4: nibbles */
    case 12u: tile_size = 32u; break;                     /* ETC1: 4x 8B subtiles */
    case 13u: tile_size = 64u; break;                     /* ETC1A4: 4x 16B subtiles */
    default: return 0;
    }
    static const uint32_t xlut[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
    static const uint32_t ylut[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
    uint32_t tile = ((y >> 3) * (w >> 3) + (x >> 3)) * tile_size;
    uint32_t morton = xlut[x & 7u] + ylut[y & 7u];
    if ((uint64_t)mapped + tile + tile_size > GUEST_RAM_SIZE) return 0;
    const uint8_t *tilep = ram + mapped + tile;
    *r = *g = *b = 255u; *a = 255u;
    /* ETC1/ETC1A4 (Azahar texture_decode.cpp + etc1.cpp): each 8x8 tile is
       four 4x4 subtiles; ETC1A4 stores 8 bytes of 4-bit alpha first. */
    if (fmt == 12u || fmt == 13u) {
        /* Subtile index must use IN-TILE coords (x%8, y%8) exactly like
           azahar LookupTexelInTile, which receives fine_x/fine_y; using the
           global x/y made every tile past column/row 0 read the wrong
           subtile (the "diagonal shift" on larger sizes). */
        uint32_t subt = ((x & 7u) >> 2) + 2u * ((y & 7u) >> 2);
        uint32_t stx = x & 3u, sty = y & 3u;
        uint32_t subt_size = (fmt == 13u) ? 16u : 8u;
        const uint8_t *sp = tilep + subt * subt_size;
        if (fmt == 13u) {
            uint64_t pa; memcpy(&pa, sp, 8);
            uint32_t nib = (uint32_t)((pa >> (4u * (stx * 4u + sty))) & 0xFu);
            *a = (uint8_t)((nib << 4) | nib);
            sp += 8;
        }
        {
            uint64_t sd; memcpy(&sd, sp, 8);
            /* ETC1Tile::GetRGB (azahar etc1.cpp), bit layout on LE u64. */
            static const int modtab[8][2] = {
                {2,8},{5,17},{9,29},{13,42},{18,60},{24,80},{33,106},{47,183}};
            uint32_t texel = 4u * stx + sty;      /* pre-swap index */
            uint32_t ex = stx, ey = sty;
            int br, bg, bb;
            if ((sd >> 32) & 1u) { uint32_t t = ex; ex = ey; ey = t; } /* flip */
            if ((sd >> 33) & 1u) { /* differential: 5-bit base + s3 delta */
                br = (int)((sd >> 59) & 31u);
                bg = (int)((sd >> 51) & 31u);
                bb = (int)((sd >> 43) & 31u);
                if (ex >= 2u) {
                    br += (int)((sd >> 56) & 7u) - (int)(((sd >> 58) & 1u) * 8u);
                    bg += (int)((sd >> 48) & 7u) - (int)(((sd >> 50) & 1u) * 8u);
                    bb += (int)((sd >> 40) & 7u) - (int)(((sd >> 42) & 1u) * 8u);
                }
                /* Azahar Convert5To8 takes u8: the (base+delta) int is
                   truncated to 8 bits BEFORE the expand — mirror exactly. */
                {
                    uint8_t rb = (uint8_t)br, gb = (uint8_t)bg, bb8 = (uint8_t)bb;
                    *r = (uint8_t)((rb << 3) | (rb >> 2));
                    *g = (uint8_t)((gb << 3) | (gb >> 2));
                    *b = (uint8_t)((bb8 << 3) | (bb8 >> 2));
                }
            } else { /* separate: two 4-bit colors */
                uint32_t base = (ex < 2u) ? 60u : 56u; /* r1 at 60, r2 at 56 */
                uint32_t rv = (uint32_t)((sd >> base) & 15u);
                uint32_t gv = (uint32_t)((sd >> (base - 8u)) & 15u);
                uint32_t bv = (uint32_t)((sd >> (base - 16u)) & 15u);
                *r = (uint8_t)((rv << 4) | rv);
                *g = (uint8_t)((gv << 4) | gv);
                *b = (uint8_t)((bv << 4) | bv);
            }
            {
                unsigned table = (unsigned)((sd >> (ex < 2u ? 37u : 34u)) & 7u);
                int m = modtab[table][(sd >> texel) & 1u];
                if ((sd >> (16u + texel)) & 1u) m = -m;
                int rr = (int)*r + m, gr = (int)*g + m, brr = (int)*b + m;
                *r = (uint8_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
                *g = (uint8_t)(gr < 0 ? 0 : gr > 255 ? 255 : gr);
                *b = (uint8_t)(brr < 0 ? 0 : brr > 255 ? 255 : brr);
            }
        }
        return 1;
    }
    /* Nibble formats (I4/A4): morton index picks the nibble in the byte. */
    if (fmt == 10u || fmt == 11u) {
        const uint8_t *p = tilep + morton / 2u;
        uint32_t nib = (morton & 1u) ? ((*p & 0xF0u) >> 4) : (*p & 0x0Fu);
        uint8_t v8 = (uint8_t)((nib << 4) | nib);
        if (fmt == 10u) { *r = *g = *b = v8; *a = 255u; }
        else { *r = *g = *b = 0u; *a = v8; }
        return 1;
    }
    {
        uint32_t bpp = (fmt == 0u) ? 4u : (fmt == 1u) ? 3u : (fmt <= 6u) ? 2u : 1u;
        const uint8_t *p = tilep + morton * bpp;
        if (fmt == 0u) { *r = p[3]; *g = p[2]; *b = p[1]; *a = p[0]; }
        else if (fmt == 1u) { *r = p[2]; *g = p[1]; *b = p[0]; }
        else if (fmt == 2u) { /* RGB5A1 */
            uint16_t v16; memcpy(&v16, p, 2);
            *r = (uint8_t)((((v16 >> 11) & 31u) << 3) | (((v16 >> 11) & 31u) >> 2));
            *g = (uint8_t)((((v16 >> 6) & 31u) << 3) | (((v16 >> 6) & 31u) >> 2));
            *b = (uint8_t)((((v16 >> 1) & 31u) << 3) | (((v16 >> 1) & 31u) >> 2));
            *a = (v16 & 1u) ? 255u : 0u;
        } else if (fmt == 3u) { /* RGB565: Azahar Convert5To8/Convert6To8
                                   ((v<<3)|(v>>2), (v<<2)|(v>>4)) — NOT v*255/n,
                                   which is off by 1 for many codes. */
            uint16_t v16; memcpy(&v16, p, 2);
            uint32_t r5 = (v16 >> 11) & 31u, g6 = (v16 >> 5) & 63u, b5 = v16 & 31u;
            *r = (uint8_t)((r5 << 3) | (r5 >> 2));
            *g = (uint8_t)((g6 << 2) | (g6 >> 4));
            *b = (uint8_t)((b5 << 3) | (b5 >> 2));
        } else if (fmt == 4u) { /* RGBA4 */
            uint16_t v16; memcpy(&v16, p, 2);
            *r = (uint8_t)((((v16 >> 12) & 15u) << 4) | ((v16 >> 12) & 15u));
            *g = (uint8_t)((((v16 >> 8) & 15u) << 4) | ((v16 >> 8) & 15u));
            *b = (uint8_t)((((v16 >> 4) & 15u) << 4) | ((v16 >> 4) & 15u));
            *a = (uint8_t)(((v16 & 15u) << 4) | (v16 & 15u));
        } else if (fmt == 5u) {
            *r = *g = *b = p[1]; *a = p[0];
        } else if (fmt == 6u) { /* RG8 */
            *r = p[1]; *g = p[0]; *b = 0u; *a = 255u;
        } else if (fmt == 7u) {
            *r = *g = *b = p[0];
        } else if (fmt == 8u) { /* A8: Azahar returns RGB=(0,0,0), not white. */
            *r = *g = *b = 0u;
            *a = p[0];
        } else if (fmt == 9u) { /* IA4 */
            uint32_t i4 = (*p >> 4) & 15u, a4 = *p & 15u;
            *r = *g = *b = (uint8_t)((i4 << 4) | i4);
            *a = (uint8_t)((a4 << 4) | a4);
        } else {
            return 0;
        }
    }
    return 1;
}

static uint32_t gsp_dma_log[8][3]; /* src, dest, size */
static unsigned gsp_dma_log_n;

/* PICA_GL_RUNTIME */
/* Diagnostic: find a guest text needle anywhere in RAM, so "the message table
   was loaded" can be told apart from "the message text was materialized for
   drawing". Gated by DQ8_SCAN_MSG=<ascii needle>; called every 500 draws once
   the guest is drawing (so it cannot fire before the first frames exist). */
static void dq8_scan_ram_text(const char *needle) {
    size_t n = strlen(needle);
    size_t i;
    unsigned hits = 0;
    uint32_t a;
    if (!n || n + 1u >= 64u || GUEST_RAM_SIZE < n + 1u) return;
    for (a = 0x1000u; a + n <= GUEST_RAM_SIZE; ++a) {
        if (ram[a] != (uint8_t)needle[0]) continue;
        if (memcmp(ram + a, needle, n) != 0) continue;
        fprintf(stderr, "RAMSCAN ascii addr=%08X tail=", a);
        for (i = 0; i < n + 8u && a + i < GUEST_RAM_SIZE; ++i) {
            uint8_t c = ram[a + i];
            fputc((c >= 32u && c < 127u) ? (int)c : '.', stderr);
        }
        fputc('\n', stderr);
        if (++hits >= 6u) break;
        a += (uint32_t)n - 1u;
    }
    for (a = 0x1000u; a + n * 2u <= GUEST_RAM_SIZE; ++a) {
        int wide = 1;
        for (i = 0; i < n; ++i)
            if (ram[a + i * 2u] != (uint8_t)needle[i] || ram[a + i * 2u + 1u] != 0u) {
                wide = 0;
                break;
            }
        if (!wide) continue;
        fprintf(stderr, "RAMSCAN utf16 addr=%08X needle=%s\n", a, needle);
        if (++hits >= 12u) break;
        a += (uint32_t)n * 2u - 1u;
    }
    if (!hits) fprintf(stderr, "RAMSCAN no hit needle=%s\n", needle);
}

/* ---------------- IDLE THROTTLE with event-driven wake -------------------
   A static game state (save dialog, title waiting for input, paused menu)
   redraws a near-identical scene and polls input; the frozen runtime burned
   a full core doing so.  This block auto-detects such a state per-state and
   throttles the runtime's own pacing while it holds, waking immediately on
   every real event (HID edge, guest deadline, audio refill need, scene
   change).  It is mode-independent: scene identity is read from the guest
   RAM of the shown scan-out slot (both screens), so the detector behaves
   identically headless and windowed; the SDL present path honours
   host_shadow_dirty so a static window is not re-presented at 60 Hz.

   arm   = scan-out BYTE-IDENTICAL for DQ8_IDLE_HOLD_FRAMES vblanks
           (DQ8_IDLE_MAX_CHANGED_WORDS, default 0 = not one word may differ)
           AND no HID edge for the same window AND no live audio source AND
           no held input AND the guest-blocked rail: the wake-window ratio is
           recorded on every window; ARM uses DQ8_IDLE_ARM_RATIO (default 0 =
           off: the dialog itself measures blocked=0.50..0.78 headless but
           only 0.06..0.28 windowed because un-armed waits are 16 ms polls, so
           a blocking arm threshold cannot pass in both modes), while RELEASE
           fires on DQ8_IDLE_BUSY_RATIO (default 0.02) = the guest runnable
           in essentially every sampled wake (animating boot saturates at
           0.00).  A >= 300 ms host-wake window must exist (>=1 completed).
           t_6ee2859c: the old arm tolerated <=256 changed words, so a screen
           whose ONLY motion is a small blinking cursor (save dialog, name
           entry) armed and then froze for 250 ms - a measured quarter-second
           hitch every ~5 s.  A screen that animates at all is not idle; the
           tolerance is now 0 and an input edge inside the hold window also
           blocks the arm.
   hold  = GSP vblank deliveries spaced by idle_period_ns, auto-tuned by
           the in-state main-thread CPU tuner (x1.5 / x0.75 toward
           DQ8_IDLE_TARGET, from DQ8_IDLE_VBLANK_START_NS up to the
           DQ8_IDLE_VBLANK_NS ceiling); a positive guest wait is rounded up to
           DQ8_IDLE_MIN_WAIT_NS only when it is ALREADY longer than one vblank,
           and never past IDLE_STRETCH_MAX_NS (2 vblanks = 33.3 ms), so a
           16.67 ms poll is delivered unchanged and no stretched wait can cost
           more than one dropped frame (the 250 ms..5 s spacings live in the
           scan-out cadence only); pad-event signalling is suppressed for
           unchanged HID ring entries; SDL presents are skipped while the
           shadow is unchanged.
   release = HID edge (idle_note_input: also pulls the frame deadline due
           now), live audio source, scene change, or sustained guest
           activity (blocked ratio below DQ8_IDLE_BUSY_RATIO).

   Zero-timeout polls and indefinite waits are never stretched, and a
   SIGNALED wait still completes immediately through guest_notify_handle,
   so delivery latency never depends on the stretch.  Runnable guest
   threads are never parked: the stretch only rounds positive wait
   durations, and guest deadlines/DSP quanta still flow through
   guest_next_wait_deadline.

   DQ8_IDLE_THROTTLE=0 restores the frozen behaviour byte-for-byte. */
static int idle_throttle_enabled_env(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("DQ8_IDLE_THROTTLE");
        enabled = (e && e[0] == '0' && !e[1]) ? 0 : 1;
        /* DETERMINISTIC (t_115d297c): the idle throttle stretches the vblank
           period from measured host CPU time, i.e. it makes guest-visible
           frame pacing depend on host load.  It is disabled in deterministic
           mode so the chain always runs at the fixed 60 Hz period. */
        if (dq8_det_mode) enabled = 0;
    }
    return enabled;
}
static uint64_t idle_env_ns(const char *name, uint64_t fallback) {
    const char *e = getenv(name);
    if (!e || !e[0]) return fallback;
    return (uint64_t)strtoull(e, 0, 0);
}
static double idle_env_ratio(const char *name, double fallback) {
    const char *e = getenv(name);
    if (!e || !e[0]) return fallback;
    return strtod(e, 0);
}
static uint32_t gsp_transfer_bytes_per_pixel(uint32_t format); /* defined below */
static int idle_armed;
static uint64_t idle_period_ns;        /* vblank spacing while armed */
static uint64_t idle_static_frames;    /* consecutive near-static scan-outs */
static uint32_t idle_held_buttons;     /* last published HID button state */
static uint64_t idle_wake_total, idle_wake_blocked, idle_windows_done;
static double idle_blocked_ratio;
static uint64_t idle_tuner_last_ns;
static uint32_t idle_tuner_prev_ticks;
static uint64_t idle_arms, idle_releases, idle_armed_total_ns, idle_present_skipped;
static uint64_t idle_armed_since_ns;
static uint64_t idle_last_input_ns;   /* last published HID change (arm gate) */
static uint64_t idle_last_retired;    /* guest steps at the previous host wake */
static int host_shadow_dirty = 1;      /* SDL present: shadow changed since last present */
static uint8_t *idle_prev_fb[2][2];    /* [screen][slot]: the guest double-buffers
                                          the scan-out and alternates the shown
                                          slot every vblank, so identity must be
                                          tracked per slot, not per screen */
static uint32_t idle_prev_bytes[2][2], idle_prev_addr[2][2], idle_prev_stride[2][2], idle_prev_fmt[2][2];
static uint32_t idle_last_changed_words; /* DQ8_IDLE_TRACE diagnostics */
static int idle_last_ok;
static uint64_t idle_pump_count;         /* host-pump iterations (trace) */
static double idle_last_tuner_cpu;       /* last tuner measurement (trace) */

static int idle_trace(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("DQ8_IDLE_TRACE") != NULL;
    return trace;
}
/* t_6ee2859c: a stretched wait must stay invisible.  One 3DS vblank is
   16.67 ms; a round-up may add at most one more of them, so no stretched
   positive wait can ever exceed 2 vblanks (33.3 ms) - a hitch smaller than a
   single dropped frame.  The 250 ms..5 s spacings belong to the scan-out
   cadence (idle_period_ns) and never to a guest wait. */
#define IDLE_VBLANK_NS 16666667ull
#define IDLE_STRETCH_MAX_NS (2ull * IDLE_VBLANK_NS)
static uint64_t idle_min_wait_ns(void) { return idle_env_ns("DQ8_IDLE_MIN_WAIT_NS", IDLE_STRETCH_MAX_NS); }
static uint64_t idle_vblank_start_ns(void) { return idle_env_ns("DQ8_IDLE_VBLANK_START_NS", 250000000ull); }
static uint64_t idle_vblank_max_ns(void) { return idle_env_ns("DQ8_IDLE_VBLANK_NS", 5000000000ull); }
static double idle_target(void) { return idle_env_ratio("DQ8_IDLE_TARGET", 0.03); }
static double idle_busy_ratio(void) { return idle_env_ratio("DQ8_IDLE_BUSY_RATIO", 0.02); }
static double idle_arm_ratio(void) { return idle_env_ratio("DQ8_IDLE_ARM_RATIO", 0.0); }
static uint32_t idle_hold_frames(void) { return (uint32_t)idle_env_ns("DQ8_IDLE_HOLD_FRAMES", 600u); }
static uint32_t idle_max_changed_words(void) { return (uint32_t)idle_env_ns("DQ8_IDLE_MAX_CHANGED_WORDS", 0u); }

static void idle_throttle_release(const char *reason); /* fwd: HID/audio hooks */

/* Process-leader CPU ticks.  /proc/<pid>/task/<pid>/stat is the per-THREAD
   view; /proc/<pid>/stat aggregates the whole thread group (measured 21.0%
   vs the leader's 13.6% on the idle dialog), so the tuner must read the
   task file exactly like the acceptance harness does. */
static uint32_t idle_main_ticks(void) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", (int)getpid());
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[256];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *close_p = strrchr(buf, ')');
    if (!close_p) return 0;
    /* after ") ": state ppid pgrp session tty_nr tpgid flags minflt cminflt
       majflt cmajflt utime stime ... -> utime is the 12th token (index 11),
       matching the harness's rest[11]/rest[12]. */
    char *p = close_p + 2;
    unsigned idx = 0;
    uint32_t utime = 0, stime = 0;
    while (*p && idx <= 12) {
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p) {
            /* Non-numeric token (the state flag, e.g. "S"): skip it but keep
               the token index so utime/stime still land at 11/12. */
            while (*p && *p != ' ') ++p;
            while (*p == ' ') ++p;
            ++idx;
            continue;
        }
        if (idx == 11) utime = (uint32_t)v;
        if (idx == 12) { stime = (uint32_t)v; break; }
        p = end;
        ++idx;
    }
    return utime + stime;
}

/* Live audio = any DSP source enabled or an unbalanced start/finish count.
   Music at a menu makes the state NOT idle (acceptance: no audio gaps). */
static int idle_audio_live(void) {
    if (!dsp_audio_on) return 0;
    for (unsigned i = 0; i < DSP_NUM_SOURCES; ++i)
        if (dsp_music_state[i].enabled || dsp_sfx_state[i].enabled) return 1;
    if (dsp_sources_started != dsp_sources_finished) return 1;
    return 0;
}

/* Called from native_hid_pump() on every call: the arming gate needs the
   held state, and a real edge must release the throttle at once.  A change in
   the published state is also an INPUT EDGE for the arm gate: a screen the
   player is interacting with is not idle. */
static void idle_note_input_state(uint32_t buttons) {
    if (buttons != idle_held_buttons) {
        idle_held_buttons = buttons;
        idle_last_input_ns = host_monotonic_ns();
    }
}
static void idle_note_input(void) {
    idle_last_input_ns = host_monotonic_ns();
    if (idle_armed) idle_throttle_release("input");
}

static uint64_t idle_stretch_wait(uint64_t ns) {
    if (split_render_enabled()) return ns; /* wall-clock vblanks: never stretch */
    if (!idle_armed || !ns) return ns;
    /* t_6ee2859c: only a wait that is ALREADY longer than one vblank may be
       rounded up, and the round-up is capped at 2 vblanks (33.3 ms).  A
       16.67 ms poll is therefore delivered unchanged, and a stretched wait can
       cost at most one dropped frame instead of the old 100 ms floor (which
       turned every poll into a visible stall). */
    if (ns <= IDLE_VBLANK_NS) return ns;
    uint64_t floor_ns = idle_min_wait_ns();
    if (floor_ns > IDLE_STRETCH_MAX_NS) floor_ns = IDLE_STRETCH_MAX_NS;
    return ns < floor_ns ? floor_ns : ns;
}
static int idle_throttle_armed(void) { return idle_armed; }

static void idle_stats_dump(void) {
    fprintf(stderr,
            "idle throttle: arms=%llu releases=%llu armed_s=%.1f presents_skipped=%llu\n",
            (unsigned long long)idle_arms, (unsigned long long)idle_releases,
            (double)idle_armed_total_ns / 1e9, (unsigned long long)idle_present_skipped);
}

static void idle_throttle_arm(uint64_t now) {
    if (idle_armed) return;
    static int dump_registered;
    if (!dump_registered) {
        dump_registered = 1;
        atexit(idle_stats_dump);
    }
    idle_armed = 1;
    idle_period_ns = idle_vblank_start_ns();
    idle_tuner_last_ns = now;
    idle_tuner_prev_ticks = idle_main_ticks();
    idle_armed_since_ns = now;
    ++idle_arms;
    if (idle_trace())
        fprintf(stderr, "idle: ARM static_frames=%llu hold=%u changed=%u no_input_ms=%.0f "
                "blocked_ratio=%.2f period=%.0fms\n",
                (unsigned long long)idle_static_frames, idle_hold_frames(),
                idle_last_changed_words,
                idle_last_input_ns ? (double)(now - idle_last_input_ns) / 1e6 : -1.0,
                idle_blocked_ratio, (double)idle_period_ns / 1e6);
}

static void idle_throttle_release(const char *reason) {
    if (!idle_armed) return;
    idle_armed = 0;
    ++idle_releases;
    idle_armed_total_ns += host_monotonic_ns() - idle_armed_since_ns;
    idle_period_ns = idle_vblank_start_ns();
    /* Pull the frame deadline due now: the guest gets its next vblank (and
       the window its present) immediately instead of after the idle period. */
    gsp_next_vblank_ns = host_monotonic_ns();
    host_shadow_dirty = 1;
    if (idle_trace())
        fprintf(stderr, "idle: RELEASE reason=%s held=%08X static_frames=%llu changed=%u "
                "period=%.0fms armed_s=%.1f\n",
                reason, idle_held_buttons, (unsigned long long)idle_static_frames,
                idle_last_changed_words, (double)idle_period_ns / 1e6,
                (double)(host_monotonic_ns() - idle_armed_since_ns) / 1e9);
}

/* Scan-out identity on the guest RAM of the shown slot for both screens
   (capture-independent, mode-independent).  Called on every PDC0 scan-out
   evaluation, armed or not, so the hold counter is fresh before arming.
   The guest double-buffers: the shown slot alternates every vblank, so each
   (screen, slot) pair keeps its own previous snapshot. */
static void idle_scanout_sample(void) {
    if (!idle_throttle_enabled_env()) return;
    uint32_t changed_words = 0;
    int ok = 1;
    for (unsigned screen = 0; screen < 2u; ++screen) {
        if (!gsp_fb_valid[screen]) { ok = 0; continue; }
        uint32_t show = gsp_fb_shown[screen] & 1u;
        uint32_t addr = gsp_fb_addr[screen][show];
        uint32_t stride = gsp_fb_stride[screen];
        uint32_t fmt = gsp_fb_fmt[screen] & 7u;
        uint32_t bpp = gsp_transfer_bytes_per_pixel(fmt);
        if (!addr || !bpp || stride < 240u * bpp || stride > 2400u * 4u) { ok = 0; continue; }
        uint32_t bytes = stride * 240u;
        uint32_t mapped;
        if (!guest_memory_range(addr, bytes, &mapped)) { ok = 0; continue; }
        if (idle_prev_fb[screen][show] && idle_prev_bytes[screen][show] == bytes &&
            idle_prev_addr[screen][show] == addr && idle_prev_stride[screen][show] == stride &&
            idle_prev_fmt[screen][show] == fmt) {
            const uint32_t *a = (const uint32_t *)(const void *)(ram + mapped);
            const uint32_t *b = (const uint32_t *)(const void *)idle_prev_fb[screen][show];
            uint32_t words = bytes / 4u, limit = idle_max_changed_words() + 1u;
            for (uint32_t i = 0; i < words; ++i)
                if (a[i] != b[i] && ++changed_words >= limit) break;
            memcpy(idle_prev_fb[screen][show], ram + mapped, bytes);
        } else {
            if (!idle_prev_fb[screen][show] || idle_prev_bytes[screen][show] < bytes) {
                free(idle_prev_fb[screen][show]);
                idle_prev_fb[screen][show] = (uint8_t *)malloc(bytes);
                idle_prev_bytes[screen][show] = idle_prev_fb[screen][show] ? bytes : 0;
            }
            if (!idle_prev_fb[screen][show]) { ok = 0; continue; }
            memcpy(idle_prev_fb[screen][show], ram + mapped, bytes);
            idle_prev_addr[screen][show] = addr;
            idle_prev_stride[screen][show] = stride;
            idle_prev_fmt[screen][show] = fmt;
            ok = 0; /* first sight of this slot is not a static frame */
        }
    }
    if (!ok) { idle_static_frames = 0; idle_last_ok = 0; idle_last_changed_words = changed_words; return; }
    idle_last_ok = 1;
    idle_last_changed_words = changed_words;
    if (changed_words <= idle_max_changed_words()) {
        ++idle_static_frames;
    } else {
        if (idle_trace())
            fprintf(stderr, "idle: scene change (%u words)\n", changed_words);
        idle_static_frames = 0;
        /* An animating scene must run at full cadence: release the throttle
           (not just the hold counter) the moment the picture changes. */
        if (idle_armed) idle_throttle_release("scene");
    }
}

/* In-state CPU tuner + activity gate, run on every host wake.  The tuning
   window is >= 3 s (DQ8_IDLE_TUNER_MS): render bursts land once per vblank
   period (~2.5-5 s), so a 1 s window sees alternating quiet/burst seconds
   and the period see-saws instead of converging. */
static void idle_tuner_tick(uint64_t now) {
    uint64_t since = now - idle_tuner_last_ns;
    uint64_t interval = idle_env_ns("DQ8_IDLE_TUNER_MS", 3000ull) * 1000000ull;
    if (since < 1000000000ull) return;
    double wall = (double)since / 1e9;
    uint32_t ticks = idle_main_ticks();
    uint32_t delta = ticks - idle_tuner_prev_ticks;
    static long clk_tck;
    if (!clk_tck) clk_tck = sysconf(_SC_CLK_TCK);
    double cpu = (double)delta / (double)clk_tck / wall;
    idle_last_tuner_cpu = cpu;
    double target = idle_target();
    uint64_t before = idle_period_ns;
    if (since < interval) {
        /* Fast path: a fresh idle state starts at ~80% main CPU; grow hard on
           every >=1 s measurement while the overshoot is large so the period
           reaches the ceiling in ~5 s (with only >=3 s windows the run's
           measurement window would catch the ramp, not the converged state).
           prev/last advance only when a path CONSUMES the measurement, so
           every cpu figure spans the whole gap since the last commit. */
        if (cpu > 4.0 * target && idle_period_ns < idle_vblank_max_ns()) {
            uint64_t cap = idle_vblank_max_ns();
            uint64_t grown = idle_period_ns * 2;
            idle_period_ns = grown > cap ? cap : grown;
            idle_tuner_last_ns = now;
            idle_tuner_prev_ticks = ticks;
            if (idle_trace())
                fprintf(stderr, "idle: TUNE cpu=%.2f%% fast-grow period %.0fms -> %.0fms\n",
                        cpu * 100.0, (double)before / 1e6, (double)idle_period_ns / 1e6);
        }
        return;
    }
    idle_tuner_last_ns = now;
    idle_tuner_prev_ticks = ticks;
    if (cpu > target) {
        uint64_t grown = (uint64_t)((double)idle_period_ns * 1.5);
        uint64_t cap = idle_vblank_max_ns();
        idle_period_ns = grown > cap ? cap : grown;
    } else if (cpu < target * 0.5 && idle_period_ns > idle_vblank_start_ns()) {
        uint64_t shrunk = (uint64_t)((double)idle_period_ns * 0.75);
        uint64_t floor_ns = idle_vblank_start_ns();
        idle_period_ns = shrunk < floor_ns ? floor_ns : shrunk;
    }
    if (idle_trace())
        fprintf(stderr, "idle: TUNE cpu=%.2f%% target=%.2f%% period %.0fms%s "
                "pumps=%llu pdc0=%llu static=%llu changed=%u\n",
                cpu * 100.0, target * 100.0, (double)idle_period_ns / 1e6,
                idle_period_ns != before ? " (changed)" : "",
                (unsigned long long)idle_pump_count,
                (unsigned long long)bench_irq_counts[2],
                (unsigned long long)idle_static_frames, idle_last_changed_words);
}

static void idle_service_host_wake(uint64_t now) {
    if (!idle_throttle_enabled_env()) return;
    /* t_6ee2859c: "busy" must mean the guest DID WORK between two host wakes,
       not "some thread is not blocked".  While armed the main context naps on
       the throttled vblank deadline and is not blocked by construction, so the
       old runnable test was true at every wake and released the throttle one
       window (~0.3 s) after every arm: arm -> 250 ms freeze -> release -> 300
       static frames -> arm again = the measured quarter-second hitch every
       ~5 s.  MAIN-context retired instructions are the honest signal: the
       guest threads' own wait loops (and the audio worker) keep their own
       counters, so a parked scene cannot look busy through them. */
    uint64_t retired = perf_main_retired;
    int worked = retired != idle_last_retired;
    idle_last_retired = retired;
    ++idle_wake_total;
    if (!worked) ++idle_wake_blocked;
    if (idle_wake_total >= 18u) { /* >= ~300 ms of host wakes at 16.67 ms */
        idle_blocked_ratio = (double)idle_wake_blocked / (double)idle_wake_total;
        idle_wake_total = idle_wake_blocked = 0;
        ++idle_windows_done;
    }
    if (idle_armed) {
        if (idle_audio_live()) { idle_throttle_release("audio"); return; }
        /* Busy release: ONLY when the guest retired instructions in
           essentially every sampled wake (DQ8_IDLE_BUSY_RATIO, default 0.02),
           i.e. it is really computing rather than parked on the throttled
           deadline.  A parked guest lands near 0.0 here (one frame per host
           250 ms window touches at most one wake in ~15).  Reset the hold
           counter so a busy scene must prove itself static again before
           re-arming. */
        if (idle_windows_done > 0 && idle_blocked_ratio <= idle_busy_ratio()) {
            idle_static_frames = 0;
            idle_throttle_release("busy");
            return;
        }
        idle_tuner_tick(now);
        return;
    }
    if (idle_trace()) {
        static uint64_t idle_state_last_ns;
        if (now - idle_state_last_ns >= 10000000000ull) {
            char runmask[128];
            size_t used = 0;
            runmask[0] = 0;
            for (unsigned i = 0; i < guest_thread_count && used + 24 < sizeof runmask; ++i)
                if (guest_threads[i].active && !guest_threads[i].blocked)
                    used += (size_t)snprintf(runmask + used, sizeof runmask - used,
                                             "%s%u:%08X", used ? "," : "", i,
                                             guest_threads[i].pc);
            idle_state_last_ns = now;
            fprintf(stderr, "idle: state armed=%d static=%llu hold=%u ok=%d changed=%u "
                    "blocked=%.2f windows=%llu held=%08X audio=%d no_input_s=%.0f runnable=%d run=[%s]\n",
                    idle_armed, (unsigned long long)idle_static_frames, idle_hold_frames(),
                    idle_last_ok, idle_last_changed_words, idle_blocked_ratio,
                    (unsigned long long)idle_windows_done, idle_held_buttons,
                    idle_audio_live(),
                    idle_last_input_ns ? (double)(now - idle_last_input_ns) / 1e9 : -1.0,
                    guest_any_runnable_thread(), runmask);
        }
    }
    /* t_6ee2859c: an input edge inside the hold window blocks the arm - a
       screen the player is interacting with is not idle, whatever the picture
       does.  The window is the same one the scan-out must stay byte-identical
       for (DQ8_IDLE_HOLD_FRAMES vblanks). */
    uint64_t hold_ns = (uint64_t)idle_hold_frames() * IDLE_VBLANK_NS;
    int input_calm = !idle_last_input_ns || now - idle_last_input_ns >= hold_ns;
    if (idle_static_frames >= idle_hold_frames() && !idle_held_buttons && input_calm &&
        !idle_audio_live() && idle_windows_done > 0 &&
        idle_blocked_ratio >= idle_arm_ratio())
        idle_throttle_arm(now);
}

/* F4/F5: wall-clock vblank generator (split mode).  The inline runtime arms
   exactly one vblank per service visit and re-anchors the deadline at "now",
   so the guest-visible PDC0/PDC1 rate depends on when the guest loop
   reaches a service point (audit F5: 31/s dialog, 49/s headless, 92/s
   movie).  This generator advances a fixed 16,666,667 ns deadline chain
   from wall-clock time and enqueues every elapsed period (bounded
   catch-up, then resync), so delivery is 60/s regardless of load. */
#define GSP_VBLANK_NS 16666667ull
#define GSP_VBLANK_CATCHUP_CAP 4u
/* PACE (t_3463e625): one wall-clock vblank chain, used by every delivery path.
   The cap bounds how many elapsed vblanks one service call may enqueue, and
   every period the chain had to skip (because the guest was away longer than
   the cap) is counted, not silently rewritten into "now".  Resynchronising to
   `now` is the last resort: it makes the guest's frame clock a function of our
   own per-frame work, which is what made the game advance at 0.7x wall time. */
static void gsp_vblank_service_period(uint64_t now, uint64_t period, unsigned cap) {
    unsigned fired = 0, resync = 0;
    if (!gsp_relay_registered || !period) return;
    if (!gsp_next_vblank_ns) {
        gsp_next_vblank_ns = now + period;
        return;
    }
    while (now >= gsp_next_vblank_ns && fired < cap) {
        gsp_next_vblank_ns += period;
        gsp_enqueue_interrupt(2u); /* PDC0: top-screen VBlank */
        gsp_enqueue_interrupt(3u); /* PDC1: bottom-screen VBlank */
        ++fired;
    }
    if (dq8_det_mode) dq8_det_vblanks += fired;
    if (now >= gsp_next_vblank_ns) {
        /* Fell further behind than the cap allows: resync.  Every dropped
           period is guest-visible frame time the chain gave away; TEMPO
           counts it (vbl_resync). */
        resync = (unsigned)((now - gsp_next_vblank_ns) / period) + 1u;
        gsp_next_vblank_ns = now + period;
    }
    tempo_vblank_seen(fired, resync);
}
static void gsp_vblank_service(uint64_t now) {
    gsp_vblank_service_period(now, GSP_VBLANK_NS, GSP_VBLANK_CATCHUP_CAP);
}

/* VBLANK WALLCLOCK (t_d1be62fc): the guest-visible vblank cadence is a
   property of the emulated hardware, so it must not be a function of how long
   OUR frame took.  The chain above is deadline-based, but it is *serviced*
   from the guest loop (guest_service_host_deadlines / the GSP relay wait), so
   while a guest frame runs for 50-80 ms no period is enqueued; the bounded cap
   then fires what it can and RESYNCS the deadline to `now`, permanently giving
   away the periods in between (TEMPO vbl_resync: 2.6 k in a 57 s slowed run,
   measured play time 0.59x wall time).

   Here a dedicated host thread owns the 60 Hz deadline chain: it advances by
   exactly one guest period per period and accumulates the elapsed periods in
   `gsp_vbl_owed` (clamped, never resynced).  The periods are *delivered* - the
   PDC0/PDC1 pair is enqueued in order - only from the context that owns the
   GSP queue (the same call sites that service vblank today), so the relay
   queue is never touched from two contexts.  A drain fires up to
   GSP_VBLANK_DRAIN_BURST periods per visit and leaves the rest owed, so a slow
   frame is caught up over the next few guest iterations instead of being
   written off.  DQ8_VBLANK_WALLCLOCK=0 restores the old in-loop chain. */
#define GSP_VBLANK_DRAIN_BURST 8u   /* periods per service visit (card: <= 8) */
/* The owed counter is clamped at the relay ring's depth in periods (32 entries
   / PDC0+PDC1 pair = 16): every period that elapses between two service visits
   is delivered on the next visit, and the guest always finds a vblank waiting
   instead of parking for a whole period.  (Clamping the *chain* would be a
   resync by another name; clamping the debt only sheds work when the guest
   services vblank so rarely that the ring could not hold the backlog - that is
   counted in gsp_vbl_owed_clamped, never rewritten into the guest's clock.) */
#define GSP_VBLANK_OWED_MAX 16u
static _Atomic unsigned gsp_vbl_owed;
static _Atomic unsigned long long gsp_vbl_owed_clamped;
static _Atomic unsigned long long gsp_vbl_pacer_periods, gsp_vbl_drained;
static unsigned long long gsp_vbl_drain_calls, gsp_vbl_drain_busy, gsp_vbl_drain_max;
static pthread_mutex_t gsp_vbl_drain_mu = PTHREAD_MUTEX_INITIALIZER;
static int gsp_vbl_pacer_started;
static int vblank_wallclock_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("DQ8_VBLANK_WALLCLOCK");
        /* Opt-in for now: the pacer is a verified win where the guest can
           actually render at the target rate (measured 59.9 vbl/s with fps and
           vbl_resync=0 on an unthrottled run), but on a single throttled core
           the *delivered* rate is bounded by the guest's own service rate, so
           the card's "vbl 60 while fps 10-20" acceptance is not met yet (see
           reports/VBLANK_WALLCLOCK_t_d1be62fc.md).  DQ8_VBLANK_WALLCLOCK=1
           enables the pacer; 0/unset keeps the old in-loop chain. */
        enabled = (e && e[0] == '1' && !e[1]) ? 1 : 0;
        /* DETERMINISTIC (t_115d297c) keeps its instruction-counted vblank: a
           wall-clock pacer would make the guest clock a function of host load
           and destroy run-to-run reproducibility. */
        if (dq8_det_mode) enabled = 0;
    }
    return enabled;
}
static int gsp_vbl_owed_pending(void) {
    return vblank_wallclock_enabled() &&
           atomic_load_explicit(&gsp_vbl_owed, memory_order_relaxed) != 0u;
}
static void gsp_vbl_sleep_ns(uint64_t ns) {
    struct timespec ts;
    if (!ns) return;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}
static void gsp_vbl_owed_add(void) {
    unsigned o = atomic_load_explicit(&gsp_vbl_owed, memory_order_relaxed);
    for (;;) {
        unsigned n;
        if (o >= GSP_VBLANK_OWED_MAX) {
            /* Only reachable when the guest itself cannot consume 60 periods a
               second (a lost host core); counted, never resynced. */
            atomic_fetch_add_explicit(&gsp_vbl_owed_clamped, 1ull, memory_order_relaxed);
            return;
        }
        n = o + 1u;
        if (atomic_compare_exchange_weak_explicit(&gsp_vbl_owed, &o, n,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            return;
    }
}
static void *gsp_vbl_pacer_main(void *arg) {
    uint64_t next = 0;
    int was_idle = 0;
    uint64_t trace_last = 0;
    (void)arg;
    for (;;) {
        uint64_t now, period, sleep_ns;
        int idle;
        if (!gsp_relay_registered) { next = 0; was_idle = 0; gsp_vbl_sleep_ns(2000000ull); continue; }
        now = host_monotonic_ns();
        idle = idle_armed ? 1 : 0;
        if (idle) {
            /* Shipped idle-throttle semantics: one guest vblank per tuned idle
               period, and no debt accrues while the scene is static. */
            period = idle_period_ns ? idle_period_ns : GSP_VBLANK_NS;
            if (!next || !was_idle) next = now + period;
            if (now >= next) { gsp_vbl_owed_add(); next = now + period; }
        } else {
            period = GSP_VBLANK_NS;
            if (was_idle) next = now;            /* leaving idle: due at once */
            else if (!next) next = now + period;
            if (!period) period = GSP_VBLANK_NS;
            while (now >= next) {                /* every elapsed period, no drop */
                gsp_vbl_owed_add();
                atomic_fetch_add_explicit(&gsp_vbl_pacer_periods, 1ull, memory_order_relaxed);
                next += period;
            }
        }
        was_idle = idle;
        atomic_store_explicit(&gsp_next_vblank_ns, next, memory_order_relaxed);
        if (getenv("DQ8_TRACE_VBLANK") && (!trace_last || now - trace_last >= 5000000000ull)) {
            trace_last = now;
            fprintf(stderr, "VBLPACER idle=%d period_ms=%.3f owed=%u owed_max_hits=%llu "
                    "pacer_periods=%llu drained=%llu calls=%llu busy=%llu max_burst=%llu "
                    "next_in_ms=%.2f\n",
                    idle, (double)period / 1e6,
                    atomic_load_explicit(&gsp_vbl_owed, memory_order_relaxed),
                    (unsigned long long)atomic_load_explicit(&gsp_vbl_owed_clamped, memory_order_relaxed),
                    (unsigned long long)atomic_load_explicit(&gsp_vbl_pacer_periods, memory_order_relaxed),
                    (unsigned long long)atomic_load_explicit(&gsp_vbl_drained, memory_order_relaxed),
                    gsp_vbl_drain_calls, gsp_vbl_drain_busy, gsp_vbl_drain_max,
                    next > now ? (double)(next - now) / 1e6 : -1.0);
        }
        now = host_monotonic_ns();
        sleep_ns = next > now ? next - now : 0;
        if (sleep_ns > 50000000ull) sleep_ns = 50000000ull; /* re-check idle state */
        if (!sleep_ns) sleep_ns = 1000000ull;
        gsp_vbl_sleep_ns(sleep_ns);
    }
    return NULL;
}
static void gsp_vbl_pacer_start(void) {
    pthread_t t;
    if (gsp_vbl_pacer_started || !vblank_wallclock_enabled()) return;
    if (!gsp_relay_registered) return;
    gsp_vbl_pacer_started = 1;
    /* Seed the chain from "now" so the first guest vblank keeps its full
       period (the pacer only ever advances this from wall-clock time). */
    if (!atomic_load_explicit(&gsp_next_vblank_ns, memory_order_relaxed))
        atomic_store_explicit(&gsp_next_vblank_ns, host_monotonic_ns() + GSP_VBLANK_NS,
                              memory_order_relaxed);
    if (pthread_create(&t, NULL, gsp_vbl_pacer_main, NULL) != 0) {
        gsp_vbl_pacer_started = 0;
        return;
    }
    pthread_detach(t);
}
/* Deliver the periods the pacer has accounted.  Called only from the GSP
   queue owner's service points (guest_service_host_deadlines and the relay
   wait), never from the pacer thread. */
static void gsp_vbl_deliver_owed(void) {
    unsigned burst = 0;
    if (!gsp_relay_registered) return;
    gsp_vbl_pacer_start();
    if (pthread_mutex_trylock(&gsp_vbl_drain_mu) != 0) { ++gsp_vbl_drain_busy; return; }
    ++gsp_vbl_drain_calls;
    while (burst < GSP_VBLANK_DRAIN_BURST) {
        unsigned o = atomic_load_explicit(&gsp_vbl_owed, memory_order_relaxed);
        if (!o) break;
        atomic_store_explicit(&gsp_vbl_owed, o - 1u, memory_order_relaxed);
        gsp_enqueue_interrupt(2u); /* PDC0: top-screen VBlank */
        gsp_enqueue_interrupt(3u); /* PDC1: bottom-screen VBlank */
        ++burst;
    }
    pthread_mutex_unlock(&gsp_vbl_drain_mu);
    if (burst > gsp_vbl_drain_max) gsp_vbl_drain_max = burst;
    if (burst) {
        atomic_fetch_add_explicit(&gsp_vbl_drained, (unsigned long long)burst, memory_order_relaxed);
        tempo_vblank_seen(burst, 0u); /* no period is resynced away on this path */
    }
}

/* F4: guest-side presentation job.  The guest thread copies the framebuffer
   out of guest RAM into an owned snapshot (never a guest pointer) and hands
   it to the present worker through a bounded SPSC queue.  Full queue = drop
   the presented frame only (policy: presentation is not guest-visible; the
   next producer event carries newer content).  The worker frees the job
   after conversion; completion travels back through the release/acquire
   done_seq (H1 contract §2 reverse transfer). */
typedef struct {
    uint64_t seq;
    uint8_t *raw;      /* owned copy of the framebuffer bytes */
    uint32_t bytes;
    uint32_t w, h, fmt;
} SplitPresentJob;
#define SPLIT_PRESENT_QUEUE_CAP 8u
static Dq8SpscQueue split_present_q;      /* guest -> worker: jobs */
static uintptr_t split_present_q_storage[SPLIT_PRESENT_QUEUE_CAP];
static uint64_t split_present_pushed, split_present_dropped;
static _Atomic uint64_t split_present_done_seq; /* last converted job seq */
static void split_present_submit(uint32_t mapped_dest, uint32_t w,
                                 uint32_t h, uint32_t format) {
    uint32_t bpp = gsp_transfer_bytes_per_pixel(format);
    uint64_t need = (uint64_t)w * h * bpp;
    SplitPresentJob *job;
    if (!bpp || mapped_dest >= GUEST_RAM_SIZE || need > GUEST_RAM_SIZE - mapped_dest)
        return;
    job = (SplitPresentJob *)calloc(1, sizeof *job);
    if (!job) { ++split_present_dropped; return; }
    job->raw = (uint8_t *)malloc((size_t)need);
    if (!job->raw) { free(job); ++split_present_dropped; return; }
    memcpy(job->raw, ram + mapped_dest, (size_t)need);
    job->bytes = (uint32_t)need;
    job->w = w; job->h = h; job->fmt = format;
    job->seq = ++split_present_pushed;
    if (!dq8_spsc_try_push(&split_present_q, (uintptr_t)job)) {
        free(job->raw);
        free(job);
        ++split_present_dropped;
        return;
    }
}

/* NATIVE_HID_RUNTIME */
/* PICA_VS_RUNTIME */

/* Command-list subroutine channels (pipeline.command_buffer, regs 0x238-0x23D;
   azahar pica_core.cpp WritePicaReg command_buffer.trigger[0/1]): size[i] =
   bytes/8 (20-bit), addr[i] = physical/8 (28-bit), and ANY write to trigger[i]
   switches execution to that buffer, abandoning the rest of the current list
   (tail call).  The previous runtime stored these registers without acting on
   them, silently dropping every chained command list (t_0b95f1a7). */
static uint32_t pica_cmdbuf_size[2], pica_cmdbuf_addr[2];
static unsigned pica_cmdbuf_depth;
/* t_da76454a: a channel trigger is a tail call (azahar cmd_list.Reset), so the
   target is latched here and pica_process_command_list continues with it. */
static uint32_t pica_tail_addr, pica_tail_size;
static void pica_process_command_list(uint32_t address, uint32_t size);

static int pica_write_register(uint32_t reg, uint32_t value, uint32_t mask) {
    if (reg == 0x200u && dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu) && value) {
        static unsigned ab_log;
        if (ab_log++ < 16u)
            fprintf(stderr, "PICA 0x200 <= %08X (field=%08X paddr=%08X)\n",
                    value, (value >> 1) & 0x0FFFFFFFu,
                    ((value >> 1) & 0x0FFFFFFFu) * 16u);
    }
    if ((reg >= 0x80u && reg <= 0x8Fu) && dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu)) {
        static unsigned tex_wr;
        if (tex_wr++ < 48u)
            fprintf(stderr, "PICA texreg %03X <= %08X mask=%X\n", reg, value, mask);
    }
    if (reg >= 0x300u) return 0;
    uint32_t old = pica_regs[reg], merged = old;
    for (unsigned byte = 0; byte < 4; ++byte)
        if (mask & (1u << byte))
            merged = (merged & ~(0xFFu << (byte * 8u))) | (value & (0xFFu << (byte * 8u)));
    pica_regs[reg] = merged;
    /* Diagnostic: distinct texture-unit-0 configurations actually written by
       guest command lists, so a texture that is bound but never drawn (or never
       bound at all) can be told apart from one that is drawn. */
    if ((reg == 0x82u || reg == 0x85u || reg == 0x8Eu) && dq8_env_on("DQ8_TRACE_TEXCFG", &dq8_env_trace_texcfg)) {
        static uint64_t seen[96];
        static unsigned nseen;
        static uint32_t seen_lr[24];
        static unsigned nseen_lr;
        uint64_t key = (uint64_t)(pica_regs[0x85] & 0x0FFFFFFFu) |
                       ((uint64_t)(pica_regs[0x8E] & 0xFu) << 28) |
                       ((uint64_t)((pica_regs[0x82] >> 16) & 0x7FFu) << 32) |
                       ((uint64_t)(pica_regs[0x82] & 0x7FFu) << 43);
        uint32_t lr = active_guest_regs ? active_guest_regs[14] : 0u;
        unsigned i;
        for (i = 0; i < nseen; ++i)
            if (seen[i] == key) break;
        if (i == nseen && nseen < sizeof(seen) / sizeof(seen[0])) {
            seen[nseen++] = key;
            fprintf(stderr, "TEXCFG addr=%08X w=%u h=%u fmt=%u via_reg=%03X\n",
                    pica_regs[0x85] & 0x0FFFFFFFu,
                    (unsigned)((pica_regs[0x82] >> 16) & 0x7FFu),
                    (unsigned)(pica_regs[0x82] & 0x7FFu),
                    (unsigned)(pica_regs[0x8E] & 0xFu), reg);
        }
        for (i = 0; i < nseen_lr; ++i)
            if (seen_lr[i] == lr) break;
        if (i == nseen_lr && nseen_lr < sizeof(seen_lr) / sizeof(seen_lr[0])) {
            seen_lr[nseen_lr++] = lr;
            fprintf(stderr, "TEXLR lr=%08X pc=%08X reg=%03X\n", lr, current_guest_pc, reg);
        }
    }
    /* VS default attributes: 0x232 index, 0x233-0x235 set_value (immediate/fixed). */
    if (reg == 0x232u) {
        vs_default_index = merged & 0xFu;
        vs_default_qi = 0;
    } else if (reg >= 0x233u && reg <= 0x235u && vs_default_index < 16u) {
        vs_default_q[vs_default_qi++] = merged;
        if (vs_default_qi == 3u) {
            pica_unpack_f24(vs_default_q, vs_default_attrib[vs_default_index]);
            if (vs_default_index < 15u) ++vs_default_index;
            vs_default_qi = 0;
        }
    }
    /* VS float-uniform queue: 0x2C0 index/format, 0x2C1-0x2C8 packed words. */
    if (reg == 0x2C0u) {
        vs_uniform_qi = 0;
    } else if (reg >= 0x2C1u && reg <= 0x2C8u) {
        int is_f32 = (pica_regs[0x2C0] >> 31) & 1u;
        unsigned need = is_f32 ? 4u : 3u;
        vs_uniform_q[vs_uniform_qi++] = value;
        if (vs_uniform_qi >= need) {
            unsigned idx = pica_regs[0x2C0] & 0x7Fu;
            if (idx < 96u) {
                if (is_f32) {
                    for (unsigned k = 0; k < 4u; ++k) {
                        float f;
                        memcpy(&f, &vs_uniform_q[k], 4);
                        vs_uniform_f[idx][3u - k] = f;
                    }
                } else {
                    /* PackedAttribute f24: 4×24-bit across 3 words. */
                    uint32_t b0 = vs_uniform_q[0], b1 = vs_uniform_q[1], b2 = vs_uniform_q[2];
                    uint32_t raw[4] = {
                        b2 & 0xFFFFFFu,
                        ((b1 & 0xFFFFu) << 8) | ((b2 >> 24) & 0xFFu),
                        ((b0 & 0xFFu) << 16) | ((b1 >> 16) & 0xFFFFu),
                        (b0 >> 8) & 0xFFFFFFu
                    };
                    for (unsigned k = 0; k < 4u; ++k)
                        vs_uniform_f[idx][k] = pica_f24_to_f32(raw[k]);
                }
            }
            if ((pica_regs[0x2C0] & 0x7Fu) < 0x7Fu)
                pica_regs[0x2C0] = (pica_regs[0x2C0] & 0xFFFFFF80u) |
                                   ((pica_regs[0x2C0] + 1u) & 0x7Fu);
            vs_uniform_qi = 0;
        }
    }
    /* VS program code: 0x2CB offset, 0x2CC-0x2D3 words. */
    if (reg == 0x2CBu) {
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu)) {
            static uint32_t shader_pcs[8];
            static unsigned nshader_pcs;
            static uint32_t shader_lrs[16];
            static unsigned nshader_lrs;
            uint32_t lr = active_guest_regs ? active_guest_regs[14] : 0u;
            unsigned i;
            for (i = 0; i < nshader_pcs; ++i)
                if (shader_pcs[i] == current_guest_pc) break;
            if (i == nshader_pcs && nshader_pcs < 8u) {
                shader_pcs[nshader_pcs++] = current_guest_pc;
                fprintf(stderr, "SHADERUPLOAD pc=%08X lr=%08X offset=%04X\n",
                        current_guest_pc, lr, value & 0xFFFFu);
            }
            for (i = 0; i < nshader_lrs; ++i)
                if (shader_lrs[i] == lr) break;
            if (i == nshader_lrs && nshader_lrs < 16u) {
                shader_lrs[nshader_lrs++] = lr;
                fprintf(stderr, "SHADERLR lr=%08X pc=%08X\n", lr, current_guest_pc);
            }
        }
        vs_program_off = value & 0xFFFFu;
    } else if (reg >= 0x2CCu && reg <= 0x2D3u && vs_program_off < 4096u) {
        vs_program[vs_program_off++] = value;
        if (vs_program_off > vs_program_size) vs_program_size = vs_program_off;
    }
    /* VS swizzle table: 0x2D5 offset, 0x2D6-0x2DD words. */
    if (reg == 0x2D5u) {
        vs_swizzle_off = value & 0xFFFFu;
    } else if (reg >= 0x2D6u && reg <= 0x2DDu && vs_swizzle_off < 4096u) {
        vs_swizzle[vs_swizzle_off++] = value;
        if (vs_swizzle_off > vs_swizzle_size) vs_swizzle_size = vs_swizzle_off;
    }
    /* PICA draw registers are write-triggered commands, not ordinary state.
       Per azahar pica_core.cpp (pipeline.trigger_draw / trigger_draw_indexed),
       ANY write triggers the draw — including a zero value.  The previous
       `&& value` gate silently dropped zero-valued trigger writes (t_0b95f1a7
       black-3D investigation: real game draw lists can trigger that way). */
    if (reg == 0x22Eu || reg == 0x22Fu) {
        ++pica_draw_count;
        dlog_note_draw();
        if (dq8_env_on("DQ8_TRACE_LAYER", &dq8_env_trace_layer)) {
            fprintf(stderr, "DRAWTRIG t=%.2f list=%llu reg=%03X value=%08X count=%u\n",
                    dq8_layer_t(), pica_list_no, reg, value, value);
        }
        {
            if (!dq8_env_scan_msg_init) {
                dq8_env_scan_msg_init = 1;
                dq8_env_scan_msg = getenv("DQ8_SCAN_MSG");
            }
            const char *scan = dq8_env_scan_msg;
            if (scan && *scan && pica_draw_count >= 600u && (pica_draw_count % 500u) == 0u)
                dq8_scan_ram_text(scan);
        }
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu)) {
            static uint32_t draw_pcs[16];
            static unsigned ndraw_pcs;
            static uint32_t draw_lrs[24];
            static unsigned ndraw_lrs;
            uint32_t lr = active_guest_regs ? active_guest_regs[14] : 0u;
            unsigned i;
            for (i = 0; i < ndraw_pcs; ++i)
                if (draw_pcs[i] == current_guest_pc) break;
            if (i == ndraw_pcs && ndraw_pcs < 16u) {
                draw_pcs[ndraw_pcs++] = current_guest_pc;
                fprintf(stderr, "DRAWPC pc=%08X lr=%08X draw=%llu\n",
                        current_guest_pc, lr, (unsigned long long)pica_draw_count);
            }
            for (i = 0; i < ndraw_lrs; ++i)
                if (draw_lrs[i] == lr) break;
            if (i == ndraw_lrs && ndraw_lrs < 24u) {
                draw_lrs[ndraw_lrs++] = lr;
                fprintf(stderr, "DRAWLR lr=%08X pc=%08X draw=%llu\n",
                        lr, current_guest_pc, (unsigned long long)pica_draw_count);
            }
            fprintf(stderr, "PICA draw trigger reg=%03X value=%08X count=%llu\n",
                    reg, value, (unsigned long long)pica_draw_count);
        }
        pica_draw_arrays(value, reg == 0x22Fu);
    }
    /* Command-buffer channels: see the state block above pica_write_register.
       Size/addr are ordinary state; the trigger switches list execution. */
    if (reg == 0x238u || reg == 0x239u) {
        pica_cmdbuf_size[reg - 0x238u] = merged & 0xFFFFFu;
        return 0;
    }
    if (reg == 0x23Au || reg == 0x23Bu) {
        pica_cmdbuf_addr[reg - 0x23Au] = merged & 0x0FFFFFFFu;
        return 0;
    }
    if (reg == 0x23Cu || reg == 0x23Du) {
        unsigned ch = reg - 0x23Cu;
        uint32_t cmdbuf_addr = pica_cmdbuf_addr[ch] * 8u;
        uint32_t cmdbuf_size = pica_cmdbuf_size[ch] * 8u;
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
            fprintf(stderr, "PICA cmdbuf trigger ch=%u addr=%08X size=%u depth=%u\n",
                    ch, cmdbuf_addr, cmdbuf_size, pica_cmdbuf_depth);
        /* t_da76454a TERRAIN FIX: a channel trigger is a TAIL CALL, not a
           nested call — azahar pica_core.cpp does `cmd_list.Reset(addr, head,
           size)`, i.e. the current list is ABANDONED and execution continues
           in the triggered buffer at the SAME nesting level.  DQ8 chains one
           command list per world object (each list ends by triggering the
           next), so the old recursive form hit its 16-level cap in the middle
           of the object chain and silently dropped every later object (the
           terrain among them: only the first 4 world draws ever executed).
           Latch the target and let pica_process_command_list continue there. */
        pica_tail_addr = cmdbuf_addr;
        pica_tail_size = cmdbuf_size;
        return 2; /* abandon the rest of the current list */
    }
    /* GPUREG_FINALIZE ends the current command list and raises the P3D
       interrupt. This register is the completion signal the guest's GSP
       relay waits for after submitting a PICA list. */
    if (reg == 0x10u && merged) {
        ++pica_finalize_count;
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
            fprintf(stderr, "PICA finalize value=%08X count=%llu -> P3D\n",
                    merged, (unsigned long long)pica_finalize_count);
        gsp_enqueue_interrupt(5u); /* P3D: command-list completion */
        return 1;
    }
    return 0;
}
static uint32_t pica_process_command_list_body(uint32_t address, uint32_t size) {
    uint32_t mapped_address;
    if ((address & 7u) || (size & 7u) || !guest_memory_range(address, size, &mapped_address)) {
        fprintf(stderr, "invalid PICA command list address=%08X size=%u\n", address, size);
        return 0;
    }
    address = mapped_address;
    uint32_t words = size / 4u, index = 0u;
    while (index + 1u < words) {
        index += index & 1u;
        if (index + 1u >= words) break;
        uint32_t value = mem32(address + index * 4u);
        uint32_t header = mem32(address + (index + 1u) * 4u);
        index += 2u;
        uint32_t reg = header & 0xFFFFu;
        uint32_t mask = (header >> 16u) & 0xFu;
        uint32_t extra = (header >> 20u) & 0x7FFu;
        uint32_t group = header >> 31u;
        if (extra > words - index) {
            fprintf(stderr, "truncated PICA command at guest=%08X reg=%03X extra=%u\n",
                    address + (index - 2u) * 4u, reg, extra);
            return 0;
        }
        { uint32_t r = (uint32_t)pica_write_register(reg, value, mask);
          if (r) return r; }
        for (uint32_t i = 0; i < extra; ++i) {
            uint32_t r = (uint32_t)pica_write_register(reg + (group ? i + 1u : 0u),
                                                       mem32(address + (index + i) * 4u), mask);
            if (r) return r;
        }
        index += extra;
    }
    return 0;
}
/* Command-list wrapper: one LAYER line per executed list (draw count, the
   render target it wrote into and the screen-space extent it touched).
   A 0x23C/0x23D channel trigger is a TAIL CALL (see pica_write_register):
   control continues in the triggered buffer instead of returning here, so the
   chain of per-object lists DQ8 submits is walked to its end.  The old
   recursive form stopped at 16 levels and dropped every later object. */
static void pica_process_command_list(uint32_t address, uint32_t size) {
    int trace = dq8_layer_trace();
    unsigned chain = 0;
    for (;;) {
        unsigned long long d0 = pica_draw_count;
        pica_bb_reset();
        ++pica_list_no;
        if (trace && chain < 512u)
            fprintf(stderr, "PLIST t=%.2f list=%llu addr=%08X size=%u d0=%llu depth=%u\n",
                    dq8_layer_t(), pica_list_no, address, size,
                    (unsigned long long)d0, chain);
        {   /* keep the last executed lists for the CMDBIN debug dump */
            uint32_t prev_addr = pica_cur_list_addr, prev_size = pica_cur_list_size;
            if (pica_list_frame_n < sizeof pica_list_frame_addr / sizeof pica_list_frame_addr[0]) {
                pica_list_frame_addr[pica_list_frame_n] = address;
                pica_list_frame_size[pica_list_frame_n] = size;
                ++pica_list_frame_n;
            }
            pica_cur_list_addr = address; pica_cur_list_size = size;
            pica_tail_addr = 0; pica_tail_size = 0;
            uint32_t r = pica_process_command_list_body(address, size);
            pica_cur_list_addr = prev_addr; pica_cur_list_size = prev_size;
            if (pica_list_frame_n) --pica_list_frame_n;
            if (trace)
                fprintf(stderr, "LAYER t=%.2f list=%llu addr=%08X size=%u draws=%llu rt=%08X %ux%u fmt=%u bbox=%u,%u..%u,%u\n",
                        dq8_layer_t(), pica_list_no, address, size,
                        (unsigned long long)(pica_draw_count - d0),
                        pica_rt_addr, pica_rt_w, pica_rt_h, pica_rt_format,
                        pica_bb_x0, pica_bb_y0, pica_bb_x1, pica_bb_y1);
            if (r != 2u) return;
            if (!pica_tail_size) return;
            if (pica_tail_addr == address && pica_tail_size == size) return; /* self-loop guard */
            if (++chain >= 4096u) {           /* runaway guard */
                fprintf(stderr, "PICA list chain exceeded 4096 tail calls at %08X\n",
                        pica_tail_addr);
                return;
            }
            address = pica_tail_addr;
            size = pica_tail_size;
        }
    }
}
static unsigned gsp_transfer_dump_index;
static uint32_t gsp_transfer_bytes_per_pixel(uint32_t format) {
    switch (format & 7u) {
        case 0u: return 4u; /* RGBA8 */
        case 1u: return 3u; /* RGB8 */
        case 2u: case 3u: case 4u: return 2u;
        default: return 0u;
    }
}
/* Host presentation of guest framebuffers (optional SDL2 dual-screen window).
   Top 400x240 and bottom 320x240 stacked in one window. Guest pixels are
   captured from GSP display-transfer destinations into RGBA8 shadow buffers
   and presented on vblank. Headless builds and DQ8_HEADLESS=1 skip all SDL. */
#define HOST_TOP_W 400u
#define HOST_TOP_H 240u
#define HOST_BOT_W 320u
#define HOST_BOT_H 240u
#define HOST_SCALE 2
static uint8_t host_shadow_top[HOST_TOP_W * HOST_TOP_H * 4u];
static uint8_t host_shadow_bot[HOST_BOT_W * HOST_BOT_H * 4u];
static int host_display_ready;
static int host_display_quit;
static int host_display_ignore_black;
static void host_display_shutdown(void);
#ifdef DQ8_ENABLE_SDL
static SDL_Window *host_sdl_window;
/* Tentative declaration: the real definition is next to the GSP queue counter
   (t_2d934e23) further down; the FPS title reads it per frame. */
static unsigned long long gsp_queue_calls;
/* Tentative: the PICA draw counters live in pica_gl.c (spliced later);
   the FPS title uses them to tell a real game frame from a repeat. */
static uint64_t pica_gl_draws_ok, pica_gl_draws_sw;
static SDL_Renderer *host_sdl_renderer;
static SDL_Texture *host_sdl_tex_top;
static SDL_Texture *host_sdl_tex_bot;
static SDL_Surface *host_sdl_surface;     /* window framebuffer (no renderer) */
static SDL_Surface *host_shadow_surf_top; /* wraps host_shadow_top pixels */
static SDL_Surface *host_shadow_surf_bot;
static int host_display_use_surface;      /* 1 = software surface path */
static int host_display_use_x11;          /* 1 = X11 SHM present (F2, no SDL surface) */
static void host_display_present(void);   /* defined below; first-present at init */
static void host_display_present_impl(void);
/* ---- F2 (PERF-P0): X11 present path for the non-GL window surface ----
   Measured on SDL 2.30/x11: SDL_GetWindowSurface() alone loads libGLX_mesa +
   libgallium and spawns the llvmpipe worker pool (23-24 threads, 43-67% of one
   core on windowed runs; PERF audit F2), while SDL_CreateWindow/SDL_PumpEvents
   stay GL-free.  So when DQ8_GL is unset we never let SDL create its window
   surface: we present through a PRIVATE X11 connection with an MIT-SHM image
   (plain XCreateImage/XPutImage fallback) and wrap the image pixels in an
   SDL_Surface, so the fill + scaled blit semantics of the old surface path are
   preserved exactly and only the final put changes.  Any failure falls back to
   the previous SDL surface path unchanged. */
#ifdef DQ8_HAVE_X11_PRESENT
static Display *host_x11_dpy;      /* private connection; SDL keeps its own */
static Window host_x11_win;
static XImage *host_x11_img;
static XShmSegmentInfo host_x11_shm;
static SDL_Surface *host_x11_wrap; /* wraps host_x11_img->data pixels */
static int host_x11_use_shm, host_x11_shm_attached;
static int host_x11_width, host_x11_height;
static int host_x11_xerror;
static int host_x11_on_error(Display *dpy, XErrorEvent *ev) {
    (void)dpy; host_x11_xerror = ev ? (int)ev->error_code : -1; return 0;
}
static void host_x11_image_destroy(void) {
    if (host_x11_wrap) { SDL_FreeSurface(host_x11_wrap); host_x11_wrap = NULL; }
    if (!host_x11_img) return;
    if (host_x11_shm_attached) {
        XShmDetach(host_x11_dpy, &host_x11_shm);
        XSync(host_x11_dpy, False);
        if (host_x11_shm.shmaddr && host_x11_shm.shmaddr != (char *)-1)
            shmdt(host_x11_shm.shmaddr);
        host_x11_shm_attached = 0;
        host_x11_img->data = NULL;
    }
    if (host_x11_img->data) { free(host_x11_img->data); host_x11_img->data = NULL; }
    XDestroyImage(host_x11_img);
    host_x11_img = NULL;
    host_x11_use_shm = 0;
}
static int host_x11_image_create(int width, int height) {
    XWindowAttributes attrs;
    host_x11_image_destroy();
    if (width <= 0 || height <= 0) return -1;
    if (!XGetWindowAttributes(host_x11_dpy, host_x11_win, &attrs)) return -1;
    if (XShmQueryExtension(host_x11_dpy)) {
        host_x11_img = XShmCreateImage(host_x11_dpy, attrs.visual,
            (unsigned)attrs.depth, ZPixmap, NULL, &host_x11_shm,
            (unsigned)width, (unsigned)height);
        if (host_x11_img) {
            host_x11_shm.shmid = shmget(IPC_PRIVATE,
                (size_t)host_x11_img->bytes_per_line * (size_t)host_x11_img->height,
                IPC_CREAT | 0600);
            host_x11_shm.shmaddr = (char *)-1;
            if (host_x11_shm.shmid >= 0)
                host_x11_shm.shmaddr = (char *)shmat(host_x11_shm.shmid, NULL, 0);
            host_x11_shm.readOnly = False;
            if (!host_x11_shm.shmaddr || host_x11_shm.shmaddr == (char *)-1) {
                if (host_x11_shm.shmid >= 0) shmctl(host_x11_shm.shmid, IPC_RMID, NULL);
                XDestroyImage(host_x11_img); /* data is still NULL */
                host_x11_img = NULL;
            } else {
                host_x11_img->data = host_x11_shm.shmaddr;
                host_x11_xerror = 0;
                XShmAttach(host_x11_dpy, &host_x11_shm);
                XSync(host_x11_dpy, False);
                shmctl(host_x11_shm.shmid, IPC_RMID, NULL); /* freed at detach */
                if (host_x11_xerror) {
                    shmdt(host_x11_shm.shmaddr);
                    host_x11_img->data = NULL;
                    XDestroyImage(host_x11_img);
                    host_x11_img = NULL;
                } else {
                    host_x11_shm_attached = 1;
                    host_x11_use_shm = 1;
                }
            }
        }
    }
    if (!host_x11_img) {
        host_x11_img = XCreateImage(host_x11_dpy, attrs.visual, (unsigned)attrs.depth,
            ZPixmap, 0, NULL, (unsigned)width, (unsigned)height, 32, 0);
        if (host_x11_img) {
            host_x11_img->data = calloc(1, (size_t)host_x11_img->bytes_per_line *
                                        (size_t)host_x11_img->height);
            if (!host_x11_img->data) {
                XDestroyImage(host_x11_img);
                host_x11_img = NULL;
            }
        }
        host_x11_use_shm = 0;
    }
    if (!host_x11_img) return -1;
    host_x11_wrap = SDL_CreateRGBSurfaceFrom(host_x11_img->data, width, height,
        host_x11_img->bits_per_pixel, host_x11_img->bytes_per_line,
        host_x11_img->red_mask, host_x11_img->green_mask, host_x11_img->blue_mask, 0);
    if (!host_x11_wrap) { host_x11_image_destroy(); return -1; }
    host_x11_width = width;
    host_x11_height = height;
    return 0;
}
static int host_x11_present_init(SDL_Window *window, int width, int height) {
    SDL_SysWMinfo info;
    memset(&info, 0, sizeof info);
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11)
        return -1;
    host_x11_win = info.info.x11.window;
    host_x11_dpy = XOpenDisplay(NULL);
    if (!host_x11_dpy) return -1;
    XSetErrorHandler(host_x11_on_error);
    if (host_x11_image_create(width, height) != 0) {
        XCloseDisplay(host_x11_dpy);
        host_x11_dpy = NULL;
        host_x11_win = 0;
        return -1;
    }
    return 0;
}
static SDL_Surface *host_x11_present_surface(SDL_Window *window) {
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    if (!host_x11_img || w != host_x11_width || h != host_x11_height) {
        if (host_x11_image_create(w, h) != 0) return NULL;
    }
    return host_x11_wrap;
}
static void host_x11_present_put(void) {
    GC gc = DefaultGC(host_x11_dpy, DefaultScreen(host_x11_dpy));
    if (host_x11_use_shm)
        XShmPutImage(host_x11_dpy, host_x11_win, gc, host_x11_img, 0, 0, 0, 0,
                     (unsigned)host_x11_img->width, (unsigned)host_x11_img->height,
                     False);
    else
        XPutImage(host_x11_dpy, host_x11_win, gc, host_x11_img, 0, 0, 0, 0,
                  (unsigned)host_x11_img->width, (unsigned)host_x11_img->height);
    XFlush(host_x11_dpy);
}
static void host_x11_present_shutdown(void) {
    if (!host_x11_dpy) return;
    host_x11_image_destroy();
    XCloseDisplay(host_x11_dpy);
    host_x11_dpy = NULL;
    host_x11_win = 0;
}
#else /* !DQ8_HAVE_X11_PRESENT */
static int host_x11_present_init(SDL_Window *window, int width, int height) {
    (void)window; (void)width; (void)height; return -1;
}
static SDL_Surface *host_x11_present_surface(SDL_Window *window) {
    (void)window; return NULL;
}
static void host_x11_present_put(void) {}
static void host_x11_present_shutdown(void) {}
#endif
static void host_display_shutdown_body(void) {
    if (!host_display_ready) return;
    host_display_ready = 0;
    host_x11_present_shutdown();
    if (host_shadow_surf_top) SDL_FreeSurface(host_shadow_surf_top);
    if (host_shadow_surf_bot) SDL_FreeSurface(host_shadow_surf_bot);
    host_shadow_surf_top = host_shadow_surf_bot = NULL;
    host_sdl_surface = NULL; /* owned by the window */
    if (host_sdl_tex_top) SDL_DestroyTexture(host_sdl_tex_top);
    if (host_sdl_tex_bot) SDL_DestroyTexture(host_sdl_tex_bot);
    if (host_sdl_renderer) SDL_DestroyRenderer(host_sdl_renderer);
    if (host_sdl_window) SDL_DestroyWindow(host_sdl_window);
    host_sdl_tex_top = host_sdl_tex_bot = NULL;
    host_sdl_renderer = NULL;
    host_sdl_window = NULL;
    SDL_Quit();
}
static void host_display_init_body(void) {
    const char *headless = getenv("DQ8_HEADLESS");
    const char *display = getenv("DQ8_DISPLAY");
    if (headless && headless[0] && !(display && display[0] == '1')) return;
    if (display && display[0] == '0') return;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "host display: SDL_Init failed: %s\n", SDL_GetError());
        return;
    }
    fprintf(stderr, "host display: SDL video driver=%s\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    host_sdl_window = SDL_CreateWindow(
        "DQ8 static recomp",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        HOST_TOP_W * HOST_SCALE, (HOST_TOP_H + HOST_BOT_H) * HOST_SCALE,
        SDL_WINDOW_SHOWN);
    if (!host_sdl_window) {
        fprintf(stderr, "host display: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return;
    }
    /* Wayland only maps a window once it has been committed with a buffer;
       the xdg_toplevel (taskbar icon) can appear while the surface stays
       invisible until the first SDL_RenderPresent.  Explicitly show the window
       now so it is not left as a ghost, and log its state for diagnosis. */
    SDL_ShowWindow(host_sdl_window);
    {
        int wx = 0, wy = 0, ww = 0, wh = 0;
        SDL_GetWindowPosition(host_sdl_window, &wx, &wy);
        SDL_GetWindowSize(host_sdl_window, &ww, &wh);
        fprintf(stderr, "host display: window flags=0x%08X pos=%d,%d size=%dx%d\n",
                (unsigned)SDL_GetWindowFlags(host_sdl_window), wx, wy, ww, wh);
    }
    /* Presentation WITHOUT Mesa (F2): SDL_GetWindowSurface loads
       libGLX_mesa/libgallium and spawns the llvmpipe worker pool even though
       we only ever blit in software (measured: 23-24 pool threads, 43-67% of
       one core in windowed runs), so we present through the X11 SHM path
       instead and never touch SDL's window surface.
       This is REQUIRED for DQ8_GL too (t_77ac52ce): SDL_GetWindowSurface on
       x11 runs a GLX MakeCurrent on this thread and sets SDL_WINDOW_OPENGL on
       the window (flags 0x4 -> 0x6).  A GLX context being current makes the
       GL backend's EGL surfaceless eglMakeCurrent fail with EGL_NOT_INITIALIZED
       (0x3002) — reproduced in a 20-line SDL+EGL test — so pica_gl_init()
       returns 0, pgl_ready stays 0 and EVERY draw falls back to the software
       rasterizer (gl_draws=0, ~2 fps in a windowed launch).  The X11 SHM path
       never creates a GL surface, so the EGL surfaceless context stays clean.
       SDL_CreateRenderer is also avoided: an OPENGL-capable driver
       (x11/wayland/offscreen) secretly initializes Mesa GL and sets
       SDL_WINDOW_OPENGL on the window (0x4 -> 0x6); those GL calls then run
       against whichever context is current on this thread — i.e. against the
       GL backend's context — which corrupted it (stale errors, wiped render
       target = black window).  A renderer is only created when the driver
       provides no presentable surface at all. */
    {
        if (host_x11_present_init(host_sdl_window, HOST_TOP_W * HOST_SCALE,
                                  (HOST_TOP_H + HOST_BOT_H) * HOST_SCALE) == 0) {
            host_display_use_x11 = 1;
            host_sdl_surface = host_x11_wrap;
            fprintf(stderr, "host display: X11 %s present path "
                    "(no SDL window surface, no SDL GL/GLX) %dx%d\n",
                    host_x11_use_shm ? "SHM" : "XPutImage",
                    host_x11_width, host_x11_height);
        } else {
            host_sdl_surface = SDL_GetWindowSurface(host_sdl_window);
        }
    }
    if (host_sdl_surface) {
        host_display_use_surface = 1;
        if (!host_display_use_x11)
            fprintf(stderr, "host display: window-surface presentation "
                    "(no SDL renderer, no GL) format=0x%08X %dx%d\n",
                    (unsigned)host_sdl_surface->format->format,
                    host_sdl_surface->w, host_sdl_surface->h);
        host_shadow_surf_top = SDL_CreateRGBSurfaceFrom(host_shadow_top,
            HOST_TOP_W, HOST_TOP_H, 32, HOST_TOP_W * 4,
            0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
        host_shadow_surf_bot = SDL_CreateRGBSurfaceFrom(host_shadow_bot,
            HOST_BOT_W, HOST_BOT_H, 32, HOST_BOT_W * 4,
            0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
        if (!host_shadow_surf_top || !host_shadow_surf_bot) {
            fprintf(stderr, "host display: shadow surface failed: %s "
                    "— falling back to an SDL renderer\n", SDL_GetError());
            if (host_shadow_surf_top) SDL_FreeSurface(host_shadow_surf_top);
            if (host_shadow_surf_bot) SDL_FreeSurface(host_shadow_surf_bot);
            host_shadow_surf_top = host_shadow_surf_bot = NULL;
            host_sdl_surface = NULL;
            host_display_use_surface = 0;
            if (host_display_use_x11) {
                host_x11_present_shutdown();
                host_display_use_x11 = 0;
            }
        }
    } else {
        fprintf(stderr, "host display: SDL_GetWindowSurface failed: %s "
                "— falling back to an SDL renderer\n", SDL_GetError());
    }
    if (!host_display_use_surface) {
        const char *gl_env = getenv("DQ8_GL");
        if (gl_env && gl_env[0] && gl_env[0] != '0') {
            host_sdl_renderer = SDL_CreateRenderer(host_sdl_window, -1,
                SDL_RENDERER_SOFTWARE);
            if (host_sdl_renderer)
                fprintf(stderr, "host display: DQ8_GL=1, software SDL renderer "
                        "(GL context reserved for the GL backend)\n");
        } else {
            host_sdl_renderer = SDL_CreateRenderer(host_sdl_window, -1,
                SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!host_sdl_renderer)
                host_sdl_renderer = SDL_CreateRenderer(host_sdl_window, -1, 0);
        }
        if (!host_sdl_renderer) {
            fprintf(stderr, "host display: SDL_CreateRenderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(host_sdl_window);
            host_sdl_window = NULL;
            SDL_Quit();
            return;
        }
        {
            /* Which renderer SDL actually picked (the request above can be
               overridden by the video driver) + the window flags afterwards:
               SDL_WINDOW_OPENGL (0x2) appearing means SDL initialized GL
               machinery that contends with the GL backend on this thread. */
            SDL_RendererInfo ri;
            memset(&ri, 0, sizeof ri);
            if (SDL_GetRendererInfo(host_sdl_renderer, &ri) == 0)
                fprintf(stderr, "host display: renderer=%s flags=0x%08X "
                        "window-flags=0x%08X\n", ri.name, (unsigned)ri.flags,
                        (unsigned)SDL_GetWindowFlags(host_sdl_window));
        }
        host_sdl_tex_top = SDL_CreateTexture(host_sdl_renderer, SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING, HOST_TOP_W, HOST_TOP_H);
        host_sdl_tex_bot = SDL_CreateTexture(host_sdl_renderer, SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING, HOST_BOT_W, HOST_BOT_H);
        if (!host_sdl_tex_top || !host_sdl_tex_bot) {
            fprintf(stderr, "host display: SDL_CreateTexture failed: %s\n", SDL_GetError());
            host_display_shutdown_body();
            return;
        }
    }
    host_display_ignore_black = getenv("DQ8_IGNORE_LCD_BLACK") != NULL;
    host_display_ready = 1;
    atexit(host_display_shutdown);
    /* Force the first commit/present now: on Wayland this maps the window
       immediately instead of waiting for the first guest-driven vblank, which
       can be many seconds away (or never reached).  Called on the present
       worker thread when DQ8_SPLIT_RENDER=1 (that thread owns SDL). */
    host_display_present_impl();
    fprintf(stderr, "host display: SDL2 window %dx%d shown+first-present "
            "(flags=0x%08X)\n",
            HOST_TOP_W * HOST_SCALE, (HOST_TOP_H + HOST_BOT_H) * HOST_SCALE,
            (unsigned)SDL_GetWindowFlags(host_sdl_window));
}
/* SDL event collection, extracted verbatim from the pump body so both the
   inline path and the DQ8_SPLIT_RENDER worker run the SAME handling; only
   the button-state sink differs (inline: native_hid_buttons on this
   thread; split: the atomic mask consumed by the guest pump). */
static unsigned kb_stick;   /* keyboard circle-pad bits: 1 up, 2 down, 4 left, 8 right */
static void host_display_collect_events(uint32_t *buttons) {
    SDL_Event ev;
    uint32_t collected = *buttons;
    /* GAMEPAD (SDL_GameController): left stick -> 3DS circle pad, buttons ->
       the same HID bit layout as the keyboard.  DQ8_GAMEPAD=0 disables it. */
    const char *padenv = getenv("DQ8_GAMEPAD");
    int pad_on = !(padenv && padenv[0] == '0');
    static SDL_GameController *host_pad_ctrl;
    static int host_pad_init_done;
    if (pad_on && !host_pad_init_done) {
        host_pad_init_done = 1;
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
            int n = SDL_NumJoysticks();
            for (int i = 0; i < n; ++i) {
                if (!SDL_IsGameController(i)) continue;
                host_pad_ctrl = SDL_GameControllerOpen(i);
                if (host_pad_ctrl) {
                    fprintf(stderr, "host pad: opened '%s' (stick = circle pad)\n",
                            SDL_GameControllerName(host_pad_ctrl));
                    break;
                }
            }
            if (!host_pad_ctrl) fprintf(stderr, "host pad: none detected (keyboard only)\n");
        } else {
            fprintf(stderr, "host pad: SDL_INIT_GAMECONTROLLER failed: %s\n", SDL_GetError());
        }
    }
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) host_display_quit = 1;
        if (pad_on && ev.type == SDL_CONTROLLERDEVICEADDED && !host_pad_ctrl) {
            if (SDL_IsGameController(ev.cdevice.which)) {
                host_pad_ctrl = SDL_GameControllerOpen(ev.cdevice.which);
                if (host_pad_ctrl)
                    fprintf(stderr, "host pad: hotplug '%s' (stick = circle pad)\n",
                            SDL_GameControllerName(host_pad_ctrl));
            }
        }
        if (pad_on && ev.type == SDL_CONTROLLERDEVICEREMOVED && host_pad_ctrl) {
            SDL_GameControllerClose(host_pad_ctrl);
            host_pad_ctrl = NULL;
            fprintf(stderr, "host pad: removed\n");
        }
        if (pad_on && (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP)) {
            uint32_t bit = 0;
            switch (ev.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A: bit = 1u; break;
                case SDL_CONTROLLER_BUTTON_B: bit = 2u; break;
                case SDL_CONTROLLER_BUTTON_BACK: bit = 4u; break;
                case SDL_CONTROLLER_BUTTON_START: bit = 8u; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: bit = 16u; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: bit = 32u; break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP: bit = 64u; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: bit = 128u; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: bit = 256u; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: bit = 512u; break;
                case SDL_CONTROLLER_BUTTON_X: bit = 1024u; break;
                case SDL_CONTROLLER_BUTTON_Y: bit = 2048u; break;
                case SDL_CONTROLLER_BUTTON_GUIDE: bit = 8u; break;
                default: break;
            }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) collected |= bit;
            else collected &= ~bit;
        }
#ifdef SDL_WINDOWEVENT_EXPOSED
        /* The compositor can invalidate the window surface without any
           guest-side content change; force the next present. */
        if (ev.type == SDL_WINDOWEVENT &&
            ev.window.event == SDL_WINDOWEVENT_EXPOSED)
            host_shadow_dirty = 1;
#endif
        if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            SDL_Keycode k = ev.key.keysym.sym; uint32_t bit = 0;
            switch(k) {
                case SDLK_z: case SDLK_SPACE: bit=1u;break;
                case SDLK_x: bit=2u;break;
                case SDLK_BACKSPACE:bit=4u;break;
                case SDLK_RETURN:bit=8u;break;
                case SDLK_RIGHT:bit=16u;break;case SDLK_LEFT:bit=32u;break;
                case SDLK_UP:bit=64u;break;case SDLK_DOWN:bit=128u;break;
                case SDLK_e:bit=256u;break;case SDLK_w:bit=512u;break;
                case SDLK_s:bit=1024u;break;case SDLK_a:bit=2048u;break;
            }
            if(ev.type==SDL_KEYDOWN)collected|=bit;else collected&=~bit;
            /* Keyboard stand-in for the circle pad (operator without a
               gamepad): I/J/K/L and the numpad arrows hold a fully
               deflected analog stick. */
            {
                unsigned sbit = 0;
                switch (k) {
                    case SDLK_i: case SDLK_KP_8: sbit = 1u; break;
                    case SDLK_k: case SDLK_KP_2: sbit = 2u; break;
                    case SDLK_j: case SDLK_KP_4: sbit = 4u; break;
                    case SDLK_l: case SDLK_KP_6: sbit = 8u; break;
                }
                if (sbit) {
                    if (ev.type == SDL_KEYDOWN) kb_stick |= sbit;
                    else                        kb_stick &= ~sbit;
                    if (getenv("DQ8_KBD_LOG"))
                        fprintf(stderr, "kb stick: bits=%u (I/J/K/L or numpad)\n", kb_stick);
                }
            }
            if (ev.type==SDL_KEYDOWN && (k == SDLK_ESCAPE || k == SDLK_q)) host_display_quit = 1;
            /* SAVESTATE (t_d50c9503): F5 snapshots the whole guest state to the
               default slot, F9 restores it.  Key repeat is ignored so one press
               is one operation. */
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                if (k == SDLK_F5) savestate_request(1);
                else if (k == SDLK_F9) savestate_request(2);
            }
        }
    }
    *buttons = collected;
    /* Left stick -> 3DS circle pad (+-156).  SDL's Y grows downward, the 3DS
       circle pad's grows upward, so Y is negated.  A stick move publishes
       through native_hid_pump's "changed" path. */
    if (pad_on && host_pad_ctrl) {
        int ax = SDL_GameControllerGetAxis(host_pad_ctrl, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(host_pad_ctrl, SDL_CONTROLLER_AXIS_LEFTY);
        const int dz = 6000;   /* ~18% deadzone */
        if (ax > -dz && ax < dz) ax = 0;
        if (ay > -dz && ay < dz) ay = 0;
        native_hid_circle_x = (int16_t)(ax * 156 / 32767);
        native_hid_circle_y = (int16_t)(-ay * 156 / 32767);
        if (SDL_GameControllerGetAxis(host_pad_ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000)
            collected |= 512u;
        if (SDL_GameControllerGetAxis(host_pad_ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000)
            collected |= 256u;
    } else {
        /* No gamepad: drive the circle pad from the keyboard.  Diagonals are
           scaled to 110 so the vector stays inside the pad's radius. */
        int kx = ((kb_stick & 8u) ? 156 : 0) - ((kb_stick & 4u) ? 156 : 0);
        int ky = ((kb_stick & 1u) ? 156 : 0) - ((kb_stick & 2u) ? 156 : 0);
        if (kx && ky) { kx = (kx > 0) ? 110 : -110; ky = (ky > 0) ? 110 : -110; }
        native_hid_circle_x = (int16_t)kx;
        native_hid_circle_y = (int16_t)ky;
    }
}
static void host_display_pump_events(void) {
    /* PERF-05: collect host input FIRST, publish SECOND.  The previous order
       ran native_hid_pump() before SDL_PollEvent(), so a key press that was
       already sitting in the SDL queue was published to the guest HID ring on
       the NEXT pump instead of this one - one extra host-to-HID delivery
       interval per fresh event.  Only the relative order of collection and
       publication changes; the publish itself keeps identical semantics: at
       most one due publish per call, the same throttle deadline
       (native_hid_next_update), the same ring entry layout/index wrap and the
       same pressed/released/event signalling.  native_hid_pump() is still
       called exactly once per pump_events() invocation, including when the
       display is unavailable (headless or built without SDL).
       DQ8_SPLIT_RENDER=1: SDL collection runs on the present worker and
       crosses here as a copied button mask (H1 contract §3.4); HID ring
       publication stays on this thread, unchanged. */
    if (split_render_enabled()) {
        native_hid_buttons = split_render_hid_mask();
        native_hid_pump();
        if (split_render_quit_requested()) {
            fprintf(stderr, "host display: quit requested\n");
            split_render_flush_sync();
            exit(0);
        }
        return;
    }
    if (host_display_ready) {
        uint32_t collected = native_hid_buttons;
        host_display_collect_events(&collected);
        native_hid_buttons = collected;
    }
    native_hid_pump();
    if (host_display_ready && host_display_quit) {
        fprintf(stderr, "host display: quit requested\n");
        exit(0);
    }
}
static void host_display_present_impl(void) {
    if (!host_display_ready) return;
    /* GL context hold boundary (t_86b8f1f9): SDL's blit/present below is a
       foreign GL user on this thread; drop the GL backend's context hold
       first so its calls can never run against the GL backend's objects. */
    pica_gl_context_release();
    /* IDLE THROTTLE (windowed): a present of UNCHANGED content is pure
       waste, and in windowed mode it runs through SDL's GLX/llvmpipe path
       (measured ~10% of main + 24 llvmpipe threads at 800x960).  Skip it
       whenever the feature is enabled and no producer marked the shadow
       dirty - armed or not - so a static scene cannot starve the host
       pump loop that evaluates arming.  DQ8_IDLE_THROTTLE=0 keeps the
       frozen behaviour byte-for-byte.  Unarmed dirty presents unchanged.
       F1 (t_4545f58b): the same skip applies with DQ8_ELIDE_RASTER enabled
       (the capture elision stops marking unchanged frames dirty), except
       that an LCD force-black transition must still present. */
    int black = gsp_lcd_force_black && !host_display_ignore_black;
    static int host_presented_black = -1;
    if ((idle_throttle_enabled_env() || elide_on()) && !host_shadow_dirty &&
        black == host_presented_black) {
        ++idle_present_skipped;
        return;
    }
    host_presented_black = black;
    host_shadow_dirty = 0;
    if (host_display_use_surface) {
        /* Pure software path: fill + scaled blit + update. No renderer, no
           GL library, window flags keep SDL_WINDOW_SHOWN only.  F2: on the
           X11 path the surface is the SHM image wrap and the update is a
           plain XShmPutImage/XPutImage on our private connection. */
        SDL_Surface *ws;
        if (host_display_use_x11) {
            ws = host_x11_present_surface(host_sdl_window);
            if (!ws) {
                static unsigned x11_failures;
                if (x11_failures++ < 4u)
                    fprintf(stderr, "host display: X11 present setup failed\n");
                return;
            }
        } else {
            ws = SDL_GetWindowSurface(host_sdl_window);
            if (!ws) {
                static unsigned surface_failures;
                if (surface_failures++ < 4u)
                    fprintf(stderr, "host display: SDL_GetWindowSurface failed: %s\n",
                            SDL_GetError());
                return;
            }
            host_sdl_surface = ws;
        }
        SDL_FillRect(ws, NULL, SDL_MapRGB(ws->format, 0, 0, 0));
        if (!black && host_shadow_surf_top && host_shadow_surf_bot) {
            SDL_Rect top = { 0, 0, HOST_TOP_W * HOST_SCALE, HOST_TOP_H * HOST_SCALE };
            SDL_Rect bot = {
                ((int)HOST_TOP_W - (int)HOST_BOT_W) / 2 * HOST_SCALE,
                (int)HOST_TOP_H * HOST_SCALE,
                HOST_BOT_W * HOST_SCALE, HOST_BOT_H * HOST_SCALE
            };
            SDL_BlitScaled(host_shadow_surf_top, NULL, ws, &top);
            SDL_BlitScaled(host_shadow_surf_bot, NULL, ws, &bot);
        }
        if (host_display_use_x11) {
            host_x11_present_put();
            return;
        }
        if (SDL_UpdateWindowSurface(host_sdl_window) != 0) {
            static unsigned update_failures;
            if (update_failures++ < 4u)
                fprintf(stderr, "host display: SDL_UpdateWindowSurface failed: %s\n",
                        SDL_GetError());
        }
        return;
    }
    SDL_UpdateTexture(host_sdl_tex_top, NULL, host_shadow_top, HOST_TOP_W * 4);
    SDL_UpdateTexture(host_sdl_tex_bot, NULL, host_shadow_bot, HOST_BOT_W * 4);
    SDL_SetRenderDrawColor(host_sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(host_sdl_renderer);
    if (!black) {
        SDL_Rect top = { 0, 0, HOST_TOP_W * HOST_SCALE, HOST_TOP_H * HOST_SCALE };
        SDL_Rect bot = {
            ((int)HOST_TOP_W - (int)HOST_BOT_W) / 2 * HOST_SCALE,
            (int)HOST_TOP_H * HOST_SCALE,
            HOST_BOT_W * HOST_SCALE, HOST_BOT_H * HOST_SCALE
        };
        SDL_RenderCopy(host_sdl_renderer, host_sdl_tex_top, NULL, &top);
        SDL_RenderCopy(host_sdl_renderer, host_sdl_tex_bot, NULL, &bot);
    }
    SDL_RenderPresent(host_sdl_renderer);
}
#else
static void host_display_shutdown_body(void) { (void)host_display_ready; }
static void host_display_init_body(void) {}
static void host_display_pump_events(void) { native_hid_pump(); }
static void host_display_present_impl(void) {}
#endif
/* Guest-thread wrappers.  With DQ8_SPLIT_RENDER=1 the worker thread calls
   the *_impl/body functions directly; the guest-thread entry points become
   no-ops for present (the worker paces it) while HID stays on this thread. */
static void host_display_present(void) {
    if (split_render_enabled()) return;
    PF_ENTER(PF_PRESENT);
    host_display_present_impl();
    PF_EXIT();
    /* GAME fps in the window title, refreshed once a second.  A "game frame" is
       a scan-out at which the guest actually drew something new: the total
       draw counter moved since the previous scan-out.  That is the rate the
       game itself advances at (30/s for this title, which is hard-locked),
       NOT the LCD refresh (60/s) and NOT the GPU-queue tick count.
       DQ8_FPS_TITLE=0 turns it off. */
    {
        static uint64_t fps_last_ns;
        static unsigned long long fps_last_game, fps_last_draws, fps_game_frames;
        static int fps_title_off = -1;
        if (fps_title_off < 0) {
            const char *e = getenv("DQ8_FPS_TITLE");
            fps_title_off = (e && e[0] == '0');
        }
        if (!fps_title_off && host_sdl_window) {
            unsigned long long draws_now = pica_gl_draws_ok + pica_gl_draws_sw;
            uint64_t now = host_monotonic_ns();
            if (draws_now != fps_last_draws) { ++fps_game_frames; fps_last_draws = draws_now; }
            if (!fps_last_ns) { fps_last_ns = now; fps_last_game = fps_game_frames; }
            double dt = (double)(now - fps_last_ns) / 1e9;
            if (dt >= 1.0) {
                unsigned long long dg = fps_game_frames - fps_last_game;
                char title[192];
                if (dg > 0)
                    snprintf(title, sizeof title,
                             "DQ8  |  GAME %.1f fps  |  %5.1f ms/frame",
                             (double)dg / dt, 1000.0 * dt / (double)dg);
                else
                    snprintf(title, sizeof title,
                             "DQ8  |  GAME 0.0 fps (paused/loading)");
                SDL_SetWindowTitle(host_sdl_window, title);
                fps_last_ns = now; fps_last_game = fps_game_frames;
            }
        }
    }
}
static void host_display_shutdown(void) {
    if (split_render_enabled()) {
        split_render_shutdown(); /* join; SDL teardown runs on the worker */
        return;
    }
    host_display_shutdown_body();
}
static void host_display_init(void) {
    if (split_render_enabled()) {
        split_render_start(); /* the worker thread runs host_display_init_body */
        return;
    }
    host_display_init_body();
}
/* PERF-04: the host shadow buffers exist only to feed consumers — the SDL
   presentation (host_display_present) and the opt-in DQ8_CAPTURE_PPM
   snapshots.  With neither present (headless, capture off) the producer still
   converted both full screens on every display transfer and every vblank
   scan-out (33.22% self Ir in the startup profile).  Skip that conversion
   when no consumer exists.  Guest-visible behaviour is untouched: the guest
   framebuffer copy in gsp_display_transfer, the scan-out slot bookkeeping in
   gsp_scanout_capture and GSP interrupt delivery all run exactly as before.

   When a consumer IS present the conversion runs eagerly on every producer
   event, with no dirty-flag fast path: the guest can rewrite the shown buffer
   directly without a buffer swap, so shadow coherence cannot be inferred from
   swap-descriptor state.  Consumer presence is runtime-invariant here
   (host_display_ready is set once before the guest starts, DQ8_CAPTURE_PPM is
   a process-start env cached on first check), so "no consumer" means the
   shadow is never needed and there is nothing to lazily refresh.

   DQ8_CAPTURE_STATS=1 prints converted/skipped counts at exit. */
static uint64_t host_display_capture_converted, host_display_capture_skipped;
static void host_display_capture_stats_dump(void) {
    fprintf(stderr, "host display capture stats: converted=%llu skipped=%llu\n",
            (unsigned long long)host_display_capture_converted,
            (unsigned long long)host_display_capture_skipped);
}
static int gsp_ppm_capture_enabled(void) {
    static int capture_enabled = -1;
    if (capture_enabled < 0) {
        const char *e = getenv("DQ8_CAPTURE_PPM");
        capture_enabled = (e && e[0] == '1' && !e[1]) ? 1 : 0;
    }
    return capture_enabled;
}
static int host_display_shadow_consumer(void) {
    static int stats_registered;
    if (!stats_registered) {
        stats_registered = 1;
        if (getenv("DQ8_CAPTURE_STATS")) atexit(host_display_capture_stats_dump);
    }
    return host_display_ready || gsp_ppm_capture_enabled();
}
/* ---- F1 capture elision (t_4545f58b): skip the redundant shadow conversion
   and the SDL/X11 present when the guest framebuffer region is byte-identical
   to the one that produced the CURRENT shadow content.  Per-shadow (top/bot)
   last (geometry, content-hash) pair: a hit means the shadow already holds
   the exact conversion of these bytes, so re-converting and re-presenting
   would reproduce the window byte-for-byte.  Everything is hashed fresh from
   guest RAM per scan-out, so any CPU/GPU-side change invalidates it.  Gated
   by DQ8_ELIDE_RASTER like the raster cache (see pica_vs.c); =0 keeps the
   frozen behaviour byte-for-byte. */
static uint64_t host_capture_elided, host_capture_hashed;
static uint64_t host_cap_last_hash[2];
static uint64_t host_cap_last_geom[2];
static int host_cap_last_valid[2];
/* Mirror of the shadow-target dispatch in host_display_capture_transfer.
   Returns 0 = top, 1 = bottom, -1 = geometry not captured into a shadow. */
static int host_capture_target(uint32_t w, uint32_t h, int *rotate_out) {
    if (w == HOST_TOP_W && h == HOST_TOP_H) { *rotate_out = 0; return 0; }
    if (w == HOST_BOT_W && h == HOST_BOT_H) { *rotate_out = 0; return 1; }
    if (w == HOST_TOP_H && h == HOST_TOP_W) { *rotate_out = 1; return 0; }
    if (w == HOST_BOT_H && h == HOST_BOT_W) { *rotate_out = 1; return 1; }
    if ((w == 480u || w == 240u) && (h == 400u || h == 320u)) {
        *rotate_out = 0;
        return (h == 400u) ? 0 : 1;
    }
    return -1;
}
/* Conversion core (F4): the classification and pixel loops that follow are
   the original inline path verbatim, but reading from an explicit source
   span so the DQ8_SPLIT_RENDER worker can run them on an owned snapshot.
   The inline path passes guest RAM directly with src_bytes =
   GUEST_RAM_SIZE - mapped_dest, preserving the original bounds decisions. */
static void host_display_convert_span(uint8_t *src, uint64_t src_bytes,
                                      uint32_t w, uint32_t h, uint32_t format) {
    uint8_t *shadow;
    uint32_t sw, sh;
    uint32_t bpp = gsp_transfer_bytes_per_pixel(format);
    int rotate = 0;
    if (!bpp) return;
    if (w == HOST_TOP_W && h == HOST_TOP_H) {
        shadow = host_shadow_top;
        sw = HOST_TOP_W; sh = HOST_TOP_H;
    } else if (w == HOST_BOT_W && h == HOST_BOT_H) {
        shadow = host_shadow_bot;
        sw = HOST_BOT_W; sh = HOST_BOT_H;
    } else if (w == HOST_TOP_H && h == HOST_TOP_W) {
        /* Native portrait LCD: 240x400 -> rotate to landscape top. */
        shadow = host_shadow_top;
        sw = HOST_TOP_W; sh = HOST_TOP_H;
        rotate = 1;
    } else if (w == HOST_BOT_H && h == HOST_BOT_W) {
        shadow = host_shadow_bot;
        sw = HOST_BOT_W; sh = HOST_BOT_H;
        rotate = 1;
    } else if (w == 480u || w == 240u) {
        /* 2x-wide or ScaleX-scaled render target; map into top/bottom shadow. */
        if (h != 400u && h != 320u) return;
        shadow = (h == 400u) ? host_shadow_top : host_shadow_bot;
        sw = (h == 400u) ? HOST_TOP_W : HOST_BOT_W;
        sh = (h == 400u) ? HOST_TOP_H : HOST_BOT_H;
        if ((uint64_t)w * h * bpp <= src_bytes) {
            for (uint32_t y = 0; y < sh; ++y) {
                uint32_t sy = y * h / sh;
                for (uint32_t x = 0; x < sw; ++x) {
                    uint32_t sx = x * w / sw;
                    uint8_t *s = src + ((uint64_t)sy * w + sx) * bpp;
                    uint8_t *d = shadow + ((uint64_t)y * sw + x) * 4u;
                    d[0] = bpp >= 1 ? s[0] : 0;
                    d[1] = bpp >= 2 ? s[1] : d[0];
                    d[2] = bpp >= 3 ? s[2] : d[0];
                    d[3] = 255u;
                }
            }
        }
        return;
    } else {
        return;
    }
    if ((uint64_t)w * h * bpp > src_bytes) return;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *s = src + ((uint64_t)y * w + x) * bpp;
            /* Portrait 240xN is bottom-up landscape: src(x,y) -> dst(h-1-y, x) */
            uint32_t dx = rotate ? y : x;
            uint32_t dy = rotate ? (w - 1u - x) : y;
            if (dx >= sw || dy >= sh) continue;
            uint8_t *d = shadow + ((uint64_t)dy * sw + dx) * 4u;
            switch (format & 7u) {
            case 0u:
                d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];
                break;
            case 1u:
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255u;
                break;
            case 2u: {
                uint16_t v; memcpy(&v, s, 2u);
                d[0] = (uint8_t)(((v >> 11u) & 31u) * 255u / 31u);
                d[1] = (uint8_t)(((v >> 5u) & 63u) * 255u / 63u);
                d[2] = (uint8_t)((v & 31u) * 255u / 31u);
                d[3] = 255u;
                break;
            }
            case 3u: {
                uint16_t v; memcpy(&v, s, 2u);
                d[0] = (uint8_t)(((v >> 11u) & 31u) * 255u / 31u);
                d[1] = (uint8_t)(((v >> 6u) & 31u) * 255u / 31u);
                d[2] = (uint8_t)(((v >> 1u) & 31u) * 255u / 31u);
                d[3] = (v & 1u) ? 255u : 0u;
                break;
            }
            case 4u: {
                uint16_t v; memcpy(&v, s, 2u);
                d[0] = (uint8_t)(((v >> 12u) & 15u) * 17u);
                d[1] = (uint8_t)(((v >> 8u) & 15u) * 17u);
                d[2] = (uint8_t)(((v >> 4u) & 15u) * 17u);
                d[3] = (uint8_t)((v & 15u) * 17u);
                break;
            }
            default:
                d[0] = d[1] = d[2] = 0u; d[3] = 255u;
                break;
            }
        }
    }
}
static void host_display_capture_transfer(uint32_t mapped_dest, uint32_t w, uint32_t h, uint32_t format) {
    tempo_capture_seen(); /* TEMPO: guest display-transfer (rendered frame) count */
    pica_gl_flush_guest_rt(); /* capture reads the frame buffer from guest RAM */
    uint32_t bpp = gsp_transfer_bytes_per_pixel(format);
    if (!host_display_shadow_consumer()) {
        /* PERF-04: no SDL window and no PPM capture — nobody reads the
           shadow, so no pixel conversion happens at all. */
        ++host_display_capture_skipped;
        return;
    }
    if (elide_on() && bpp && mapped_dest < GUEST_RAM_SIZE &&
        (uint64_t)mapped_dest + (uint64_t)w * h * bpp <= GUEST_RAM_SIZE) {
        /* F1 capture elision (t_4545f58b): byte-identical framebuffer that
           already produced the CURRENT shadow content -> skip re-conversion
           and let the present path skip the redundant present too. */
        int rot, tgt = host_capture_target(w, h, &rot);
        if (tgt >= 0) {
            uint64_t geom = ((uint64_t)(format & 7u) << 32) |
                            ((uint64_t)w << 12) | (uint64_t)h |
                            ((uint64_t)rot << 36) | ((uint64_t)bpp << 40);
            uint64_t hash = elide_hash_bytes(14695981039346656037ull,
                                             ram + mapped_dest,
                                             (size_t)w * h * bpp);
            ++host_capture_hashed;
            if (host_cap_last_valid[tgt] && host_cap_last_geom[tgt] == geom &&
                host_cap_last_hash[tgt] == hash) {
                ++host_capture_elided;
                return;
            }
            host_cap_last_valid[tgt] = 1;
            host_cap_last_geom[tgt] = geom;
            host_cap_last_hash[tgt] = hash;
        }
    }
    ++host_display_capture_converted;
    if (split_render_enabled()) {
        /* F4: snapshot the bytes on THIS thread (guest RAM never crosses
           the queue) and let the present worker convert/present. */
        if (!bpp || mapped_dest >= GUEST_RAM_SIZE) return;
        split_present_submit(mapped_dest, w, h, format);
        return;
    }
    host_shadow_dirty = 1; /* a real producer event: the window needs a present */
    /* Framebuffer snapshotting (for the built-in /tmp PPM capture) must work
       even in headless builds that did not bring up an SDL window; only the
       SDL texture upload (host_display_present) is guarded by host_display_ready.
       (PERF-04: with BOTH consumers absent the gate above skips the whole
       conversion instead.) */
    if (!bpp || mapped_dest >= GUEST_RAM_SIZE) return;
    host_display_convert_span(ram + mapped_dest, GUEST_RAM_SIZE - mapped_dest,
                              w, h, format);
}
static void gsp_memory_fill(uint32_t *args) {
    /* t_fa78ea1f: a fill overwrites guest pixels, exactly like a display
       transfer reads them --  the deferred GL episode must land in guest RAM
       first, otherwise the fill is applied to a buffer whose GL content is
       still pending and the next pgl_refresh() has to guess which of the two
       is newer. */
    pica_gl_flush_guest_rt();
    if (dq8_layer_trace())
        fprintf(stderr, "LAYER t=%.2f fill %08X..%08X value=%08X ctl=%04X | %08X..%08X value=%08X ctl=%04X list=%llu\n",
                dq8_layer_t(), args[0], args[2], args[1], args[6] & 0xFFFFu,
                args[3], args[5], args[4], args[6] >> 16u, pica_list_no);
    uint32_t starts[2] = { args[0], args[3] };
    uint32_t values[2] = { args[1], args[4] };
    uint32_t ends[2] = { args[2], args[5] };
    uint32_t controls[2] = { args[6] & 0xFFFFu, args[6] >> 16u };
    for (unsigned b = 0; b < 2u; ++b) {
        uint32_t start, end;
        uint32_t control = controls[b];
        uint64_t bytes = ends[b] >= starts[b] ? (uint64_t)ends[b] - starts[b] : UINT64_MAX;
        if (starts[b] && ends[b] >= starts[b] &&
            guest_memory_range(starts[b], bytes, &start)) {
            end = start + (uint32_t)bytes;
            /* t_477973bb: the fill overwrites guest pixels — the FBO must be
               re-synced from them before the next draw (same contract as the
               flush above, and the reason the shadow compare exists). */
            pica_gl_note_guest_write(ram + start, end - start);
            uint32_t bytes_per_pixel = (control >> 8u) == 0u ? 2u :
                                      (control >> 8u) == 1u ? 3u : 4u;
            uint32_t pattern_bytes = bytes_per_pixel;
            uint8_t pattern[4];
            memcpy(pattern, &values[b], sizeof(pattern));
            /* Fast path (OPT-03): the validated span is a pattern memset.
               The bulk loops below write exactly the bytes
               pattern[(a - start) % pattern_bytes]; only the store shape
               changes (memcpy-based, so unaligned starts stay well-defined
               on the host), never the values, the span, the validation order
               or the PSC interrupt order.  Tails keep the per-byte phase. */
            {
                uint32_t len = end - start;
                uint8_t *dst = ram + start;
                uint32_t a = 0;
                if (pattern_bytes != 3u) {
                    /* widths 2 and 4 divide 8: one repeating 64-bit word
                       (mask, not modulo: pattern_bytes is 2 or 4 here) */
                    uint64_t word;
                    uint8_t wbytes[8];
                    for (uint32_t i = 0; i < 8u; ++i)
                        wbytes[i] = pattern[i & (pattern_bytes - 1u)];
                    memcpy(&word, wbytes, sizeof(word));
                    for (; len - a >= 8u; a += 8u)
                        memcpy(dst + a, &word, sizeof(word));
                } else {
                    /* width 3: one repeating 24-byte block (8 periods) */
                    uint8_t block[24];
                    for (uint32_t i = 0; i < 24u; ++i)
                        block[i] = pattern[i % 3u];
                    for (; len - a >= 24u; a += 24u)
                        memcpy(dst + a, block, sizeof(block));
                }
                /* tail (<8 or < 24 bytes): the chunk sizes are multiples of
                   pattern_bytes, so the phase restarts at 0 here and a
                   rolling index replaces a per-byte modulo */
                uint32_t k = 0;
                for (; a < len; ++a) {
                    dst[a] = pattern[k];
                    if (++k == pattern_bytes) k = 0u;
                }
            }
        }
        if (control & 1u)
            gsp_enqueue_interrupt((uint8_t)b); /* PSC0 / PSC1 */
    }
    if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
        fprintf(stderr, "GSP memory-fill %08X..%08X value=%08X control=%04X; %08X..%08X value=%08X control=%04X\n",
                starts[0], ends[0], values[0], controls[0], starts[1], ends[1], values[1], controls[1]);
}
/* XFERPROF (t_2d934e23): the scan-out transfer is one of the two heaviest
   shims in a spike frame (28.1 ms per frame over 2 transfers, 27 % of it).
   Split its wall time at the three real boundaries so the fix targets the
   actual cost, not a guess: (1) pica_gl_flush_guest_rt (GL readback + the
   deferred write-back of the render target into guest RAM), (2) the per-pixel
   source->dest conversion loop, (3) host_display_capture_transfer (the
   guest-RAM -> host shadow conversion that feeds the window).  Aggregate over
   a ~2 s window; one getenv per process when the knob is off. */
static int xfer_prof_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DQ8_XFERPROF"); v = (e && e[0] && e[0] != '0'); }
    return v;
}
static uint64_t xfer_prof_flush_ns, xfer_prof_loop_ns, xfer_prof_cap_ns;
static uint64_t xfer_prof_max_flush_ns, xfer_prof_max_loop_ns, xfer_prof_max_cap_ns;
static uint64_t xfer_prof_src_bytes, xfer_prof_dst_bytes;
static unsigned xfer_prof_n;
static double xfer_prof_last;
/* t_2d934e23: GSP TriggerCmdReqQueue (cmd 0x000C0000) calls — the guest's
   per-3D-frame GPU command queue, i.e. one logic/render tick.  Printed with
   the XFERPROF window so the tick rate is readable next to the transfer cost. */
static unsigned long long gsp_queue_calls;
static void xfer_prof_report(uint64_t t_flush, uint64_t t_loop, uint64_t t_cap,
                             uint32_t in_w, uint32_t in_h, uint32_t out_w,
                             uint32_t out_h, uint32_t flags) {
    double now = dq8_layer_t();
    pf_shim(PF_SH_XFER_FLUSH, t_flush);
    pf_shim(PF_SH_XFER_LOOP, t_loop);
    pf_shim(PF_SH_XFER_CAP, t_cap);
    xfer_prof_flush_ns += t_flush; xfer_prof_loop_ns += t_loop; xfer_prof_cap_ns += t_cap;
    if (t_flush > xfer_prof_max_flush_ns) xfer_prof_max_flush_ns = t_flush;
    if (t_loop > xfer_prof_max_loop_ns) xfer_prof_max_loop_ns = t_loop;
    if (t_cap > xfer_prof_max_cap_ns) xfer_prof_max_cap_ns = t_cap;
    xfer_prof_src_bytes += (uint64_t)in_w * in_h * 4u;
    xfer_prof_dst_bytes += (uint64_t)out_w * out_h * 3u;
    ++xfer_prof_n;
    if (xfer_prof_last == 0.0) { xfer_prof_last = now; return; }
    if (now - xfer_prof_last >= 2.0) {
        fprintf(stderr, "XFERPROF n=%u flush_ms=%.2f loop_ms=%.2f capture_ms=%.2f "
                "total_ms=%.2f mean_ms=%.2f max[flush/loop/cap]=%.2f/%.2f/%.2f "
                "flags=%08X queue=%llu\n",
                xfer_prof_n, (double)xfer_prof_flush_ns / 1e6,
                (double)xfer_prof_loop_ns / 1e6, (double)xfer_prof_cap_ns / 1e6,
                (double)(xfer_prof_flush_ns + xfer_prof_loop_ns + xfer_prof_cap_ns) / 1e6,
                (double)(xfer_prof_flush_ns + xfer_prof_loop_ns + xfer_prof_cap_ns) /
                    (double)xfer_prof_n / 1e6,
                (double)xfer_prof_max_flush_ns / 1e6, (double)xfer_prof_max_loop_ns / 1e6,
                (double)xfer_prof_max_cap_ns / 1e6, flags, gsp_queue_calls);
        gsp_queue_calls = 0;
        xfer_prof_flush_ns = xfer_prof_loop_ns = xfer_prof_cap_ns = 0;
        xfer_prof_max_flush_ns = xfer_prof_max_loop_ns = xfer_prof_max_cap_ns = 0;
        xfer_prof_src_bytes = xfer_prof_dst_bytes = 0;
        xfer_prof_n = 0;
        xfer_prof_last = now;
    }
}
/* t_fa78ea1f DARKLOG: last display transfer (defined with the sampler). */
/* Per-render-target draw tallies since the last top-screen display transfer,
   so a frame that presents a UI-less image can be attributed to "the guest drew
   no UI draws into the RT" vs "the backend lost them". */
static int dlog_enabled(void); /* defined with the scan-out sampler */
#define DLOG_RT_SLOTS 8
static struct { uint32_t addr, w, h; unsigned draws; } dlog_rt[DLOG_RT_SLOTS];
static unsigned dlog_rt_n;
static void dlog_note_draw(void) {
    unsigned i;
    for (i = 0; i < dlog_rt_n; ++i)
        if (dlog_rt[i].addr == pica_rt_addr) { ++dlog_rt[i].draws; return; }
    if (dlog_rt_n < DLOG_RT_SLOTS) {
        dlog_rt[dlog_rt_n].addr = pica_rt_addr;
        dlog_rt[dlog_rt_n].w = pica_rt_w;
        dlog_rt[dlog_rt_n].h = pica_rt_h;
        dlog_rt[dlog_rt_n].draws = 1;
        ++dlog_rt_n;
    }
}
/* Dark share of a colour buffer in PICA tiled layout. */
static double dlog_dark_tiled(uint32_t addr, uint32_t w, uint32_t h, uint32_t gsp_fmt) {
    uint32_t x, y, n = 0, dark = 0, fmt = gsp_fmt;
    uint8_t r, g, b, a;
    if (!addr || !w || !h) return -1.0;
    if (fmt == 2u) fmt = 3u; else if (fmt == 3u) fmt = 2u;
    for (y = 0; y < h; y += 8u) {
        for (x = 0; x < w; x += 8u) {
            if (!pica_get_pixel(addr, w, h, fmt, x, y, &r, &g, &b, &a)) return -1.0;
            unsigned m = r > g ? (r > b ? r : b) : (g > b ? g : b);
            ++n;
            if (m <= 16u) ++dark;
        }
    }
    return n ? 100.0 * (double)dark / (double)n : -1.0;
}

static uint32_t dlog_xsrc, dlog_xdst, dlog_xw, dlog_xh, dlog_xfmt, dlog_xn;
static void gsp_display_transfer(uint32_t *args) {
    int xp = xfer_prof_enabled();
    uint64_t xp_t0 = 0, xp_flush = 0, xp_loop = 0;
    if (xp) xp_t0 = host_monotonic_ns();
    pica_gl_flush_guest_rt(); /* transfer reads source pixels from guest RAM */
    if (xp) { xp_flush = host_monotonic_ns() - xp_t0; xp_t0 = host_monotonic_ns(); }
    if (dq8_layer_trace())
        fprintf(stderr, "LAYER t=%.2f xfer src=%08X in=%ux%u ifmt=%u -> dst=%08X out=%ux%u ofmt=%u flags=%08X list=%llu\n",
                dq8_layer_t(), args[0], (args[2] & 0xFFFFu), (args[2] >> 16u), (args[4] >> 8) & 7u,
                args[1], (args[3] & 0xFFFFu), (args[3] >> 16u), (args[4] >> 12) & 7u, args[4], pica_list_no);
    if (dlog_enabled() && ((args[2] >> 16u) == 400u)) {
        unsigned i;
        fprintf(stderr, "XFDARK src=%08X dst=%08X src_dark=%.1f draws=%llu rts=",
                args[0], args[1],
                dlog_dark_tiled(args[0], args[2] & 0xFFFFu, args[2] >> 16u, (args[4] >> 8) & 7u),
                (unsigned long long)pica_draw_count);
        for (i = 0; i < dlog_rt_n; ++i)
            fprintf(stderr, "%s%08X:%u", i ? " " : "", dlog_rt[i].addr, dlog_rt[i].draws);
        fputc(10, stderr);
        dlog_rt_n = 0;
    }
    uint32_t source = args[0], dest = args[1], input_dim = args[2], output_dim = args[3], flags = args[4];
    uint32_t input_w = input_dim & 0xFFFFu, input_h = input_dim >> 16u;
    uint32_t output_w = output_dim & 0xFFFFu, output_h = output_dim >> 16u;
    uint32_t input_format = (flags >> 8u) & 7u, output_format = (flags >> 12u) & 7u;
    uint32_t in_bpp = gsp_transfer_bytes_per_pixel(input_format), out_bpp = gsp_transfer_bytes_per_pixel(output_format);
    if (!input_w) input_w = output_w;
    if (!input_h) input_h = output_h;
    uint32_t scaling0 = (flags >> 24) & 3u;
    uint32_t eff_w = output_w >> ((scaling0 != 0u) ? 1 : 0);
    uint32_t eff_h = output_h >> ((scaling0 == 2u) ? 1 : 0);
    uint32_t mapped_source = 0u, mapped_dest = 0u;
    if (!input_w || !input_h || !output_w || !output_h || !in_bpp || !out_bpp ||
        output_w > 4096u || output_h > 4096u || source >= GUEST_RAM_SIZE || dest >= GUEST_RAM_SIZE ||
        !guest_memory_range(source, (uint64_t)input_w * input_h * in_bpp, &mapped_source) ||
        !guest_memory_range(dest, (uint64_t)eff_w * eff_h * out_bpp, &mapped_dest)) {
        fprintf(stderr, "invalid GSP display transfer src=%08X dim=%08X dst=%08X dim=%08X flags=%08X\n",
                source, input_dim, dest, output_dim, flags);
        return;
    }
    uint32_t guest_src = source, guest_dst = dest;
    dlog_xsrc = guest_src; dlog_xdst = guest_dst;
    dlog_xw = output_w; dlog_xh = output_h; dlog_xfmt = output_format;
    ++dlog_xn;
    source = mapped_source;
    dest = mapped_dest;
    int vflip = flags & 1u;
    int input_linear = (flags >> 1) & 1u;
    int dont_swizzle = (flags >> 5) & 1u;
    uint32_t scaling = (flags >> 24) & 3u;
    if (scaling == 3u) {
        fprintf(stderr, "GSP display transfer: reserved scaling mode 3, flags=%08X\n", flags);
        return;
    }
    uint32_t h_scale = (scaling != 0u) ? 1u : 0u;
    uint32_t v_scale = (scaling == 2u) ? 1u : 0u;
    uint32_t out_w = output_w >> h_scale;
    uint32_t out_h = output_h >> v_scale;
    if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu)) {
        static unsigned xfer_log;
        /* Every transfer: source/dest/dims, to see whether bottom uses the top RT. */
        if (xfer_log++ < 500u)
            fprintf(stderr, "XFER va=%08X->%08X va=%08X->%08X in %ux%u out %ux%u->%ux%u ifmt=%u ofmt=%u flags=%08X vflip=%d lin=%d noswz=%d scale=%u fb=%08X %ux%u f=%u\n",
                    guest_src, source, guest_dst, dest,
                    input_w, input_h, output_w, output_h, out_w, out_h,
                    input_format, output_format, flags, vflip, input_linear, dont_swizzle, scaling,
                    pica_rt_addr, pica_rt_w, pica_rt_h, pica_rt_format);
    }
    /* t_2d934e23: scan-out conversion fast path.  The per-frame display
       transfer is tiled RGBA8 -> linear RGB8 (ScaleX box filter), i.e. exactly
       this shape; the generic loop below resolves pica_get_pixel (Morton math
       + guest_memory_range) TWICE per output pixel and re-derives the fb
       format every iteration: measured 3.7 ms for 240x400 (96 k pixels,
       39 ns/pixel).  Here the Morton tile offset is computed once per row
       slice and the format conversion is inlined, so the same bytes are
       produced with a handful of loads/stores per pixel.  Byte identity is
       verified against the generic path (DQ8_XFER_FAST=0) with the
       DQ8_DUMP_TRANSFER_PREFIX PPM dumps. */
    {
        static int xfer_fast_on = -1;
        if (xfer_fast_on < 0) {
            const char *e = getenv("DQ8_XFER_FAST");
            xfer_fast_on = (e && e[0] == '0' && !e[1]) ? 0 : 1;
        }
        if (xfer_fast_on && input_format == 0u && output_format == 1u &&
            in_bpp == 4u && out_bpp == 3u && !input_linear && !dont_swizzle &&
            !vflip && v_scale == 0u && out_w <= input_w && out_h <= input_h &&
            !(input_w & 7u) && !(input_h & 7u)) {
            static const uint32_t xl[8] = {0u, 1u, 4u, 5u, 16u, 17u, 20u, 21u};
            static const uint32_t yl[8] = {0u, 2u, 8u, 10u, 32u, 34u, 40u, 42u};
            uint32_t tiles_per_row = input_w >> 3u;
            for (uint32_t y = 0; y < out_h; ++y) {
                /* v_scale == 0 so the source row is y itself */
                uint32_t trow = (y >> 3u) * tiles_per_row * 256u +
                                yl[y & 7u] * 4u;
                uint8_t *dp = ram + dest + (uint64_t)y * out_w * 3u;
                for (uint32_t x = 0; x < out_w; ++x) {
                    uint32_t sx = x << h_scale;
                    const uint8_t *p0 = ram + source + trow +
                        ((sx >> 3u) * 256u + xl[sx & 7u] * 4u);
                    uint8_t r = p0[3], g = p0[2], b = p0[1];
                    if (h_scale) {
                        const uint8_t *p1 = ram + source + trow +
                            (((sx + 1u) >> 3u) * 256u + xl[(sx + 1u) & 7u] * 4u);
                        r = (uint8_t)(((unsigned)r + p1[3]) / 2u);
                        g = (uint8_t)(((unsigned)g + p1[2]) / 2u);
                        b = (uint8_t)(((unsigned)b + p1[1]) / 2u);
                    }
                    dp[0] = b; dp[1] = g; dp[2] = r;
                    dp += 3;
                }
            }
            goto transfer_pixels_done;
        }
    }
    for (uint32_t y = 0; y < out_h; ++y) {
        uint32_t sy = y << v_scale;
        if (vflip) sy = input_h - 1u - (y << v_scale);
        for (uint32_t x = 0; x < out_w; ++x) {
            uint32_t sx = x << h_scale;
            uint8_t r = 0, g = 0, b = 0, a = 255;
            uint32_t fb_fmt = input_format;
            if (input_format == 2u) fb_fmt = 3u;
            else if (input_format == 3u) fb_fmt = 2u;
            int got = (!input_linear) &&
                pica_get_pixel(source, input_w, input_h, fb_fmt, sx, sy, &r, &g, &b, &a);
            if (!got) {
                /* Linear source: GPU RGBA8 is A,B,G,R and RGB8 is B,G,R. */
                uint8_t *src = ram + source + ((uint64_t)sy * input_w + sx) * in_bpp;
                if (in_bpp == 4u) {
                    a = src[0]; b = src[1]; g = src[2]; r = src[3];
                } else if (in_bpp == 3u) {
                    b = src[0]; g = src[1]; r = src[2]; a = 255;
                } else if (in_bpp == 2u) {
                    uint16_t v16; memcpy(&v16, src, 2);
                    if (input_format == 2u) {
                        r = (uint8_t)(((v16 >> 11) & 31) * 255 / 31);
                        g = (uint8_t)(((v16 >> 5) & 63) * 255 / 63);
                        b = (uint8_t)((v16 & 31) * 255 / 31);
                    } else r = g = b = (uint8_t)(v16 & 0xFF);
                    a = 255;
                } else { r = g = b = src[0]; a = 255; }
            }
            /* Box filter for ScaleX / ScaleXY (sw_blitter.cpp). */
            if (h_scale) {
                uint8_t r2 = r, g2 = g, b2 = b, a2 = a;
                uint32_t sx2 = sx + 1u;
                if (!pica_get_pixel(source, input_w, input_h, fb_fmt, sx2, sy, &r2, &g2, &b2, &a2)) {
                    uint8_t *src = ram + source + ((uint64_t)sy * input_w + sx2) * in_bpp;
                    if (in_bpp == 4u) { a2 = src[0]; b2 = src[1]; g2 = src[2]; r2 = src[3]; }
                    else if (in_bpp == 3u) { b2 = src[0]; g2 = src[1]; r2 = src[2]; a2 = 255; }
                    else { r2 = g2 = b2 = src[0]; a2 = 255; }
                }
                r = (uint8_t)(((unsigned)r + r2) / 2u);
                g = (uint8_t)(((unsigned)g + g2) / 2u);
                b = (uint8_t)(((unsigned)b + b2) / 2u);
            }
            if (v_scale) {
                uint8_t r2 = r, g2 = g, b2 = b, a2 = a;
                uint32_t sy2 = sy + 1u;
                if (!pica_get_pixel(source, input_w, input_h, fb_fmt, sx, sy2, &r2, &g2, &b2, &a2)) {
                    uint8_t *src = ram + source + ((uint64_t)sy2 * input_w + sx) * in_bpp;
                    if (in_bpp == 4u) { a2 = src[0]; b2 = src[1]; g2 = src[2]; r2 = src[3]; }
                    else if (in_bpp == 3u) { b2 = src[0]; g2 = src[1]; r2 = src[2]; a2 = 255; }
                    else { r2 = g2 = b2 = src[0]; a2 = 255; }
                }
                r = (uint8_t)(((unsigned)r + r2) / 2u);
                g = (uint8_t)(((unsigned)g + g2) / 2u);
                b = (uint8_t)(((unsigned)b + b2) / 2u);
            }
            /* Dest is linear when !dont_swizzle, tiled otherwise. */
            uint32_t dst_off;
            if (!dont_swizzle && !input_linear) {
                dst_off = ((uint64_t)y * out_w + x) * out_bpp;
            } else {
                static const uint32_t xlut[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
                static const uint32_t ylut[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
                dst_off = ((((y >> 3) * ((out_w + 7u) >> 3) + (x >> 3)) * 64u) +
                           xlut[x & 7u] + ylut[y & 7u]) * out_bpp;
            }
            uint8_t *dst = ram + dest + dst_off;
            if (out_bpp == 4u) {
                dst[0] = a; dst[1] = b; dst[2] = g; dst[3] = r;
            } else if (out_bpp == 3u) {
                dst[0] = b; dst[1] = g; dst[2] = r;
            } else if (out_bpp == 2u) {
                uint16_t v16 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                memcpy(dst, &v16, 2);
            } else {
                dst[0] = r;
            }
        }
    }
transfer_pixels_done:
    if (xp) { xp_loop = host_monotonic_ns() - xp_t0; xp_t0 = host_monotonic_ns(); }
    const char *prefix = getenv("DQ8_DUMP_TRANSFER_PREFIX");
    if (prefix && output_format == 1u) {
        char path[1024];
        if (getenv("DQ8_DUMP_TRANSFER_LATEST"))
            snprintf(path, sizeof(path), "%s-%s.ppm", prefix, out_h == 400u ? "top" : "bottom");
        else
            snprintf(path, sizeof(path), "%s-%u.ppm", prefix, ++gsp_transfer_dump_index);
        FILE *dump = fopen(path, "wb");
        if (dump) {
            fprintf(dump, "P6\n%u %u\n255\n", out_w, out_h);
            for (uint32_t i = 0; i < out_w * out_h; ++i) {
                uint8_t *p = ram + dest + i * 3u;
                uint8_t rgb[3] = { p[2], p[1], p[0] };
                fwrite(rgb, 1u, 3u, dump);
            }
            fclose(dump);
            fprintf(stderr, "GSP display-transfer dump: src=%08X dst=%08X %ux%u -> %s\n", guest_src, dest, out_w, out_h, path);
        }
    }
    if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
        fprintf(stderr, "GSP display-transfer src=%08X %ux%u fmt=%u -> dst=%08X %ux%u fmt=%u flags=%08X\n",
                source, input_w, input_h, input_format, dest, out_w, out_h, output_format, flags);
    /* Scan-out shield: once the guest registers framebuffer info, the LCD
       shows ONLY the currently-shown pair member. A transfer into the other
       member must not overwrite the shadow — on hardware it is invisible. */
    {
        int screen = (out_h == 400u || out_h == 800u) ? 0 : 1;
        if (gsp_fb_valid[screen] && guest_dst != gsp_fb_addr[screen][gsp_fb_shown[screen]]) {
            if (dq8_env_on("DQ8_TRACE_GSP_FB", &dq8_env_trace_gsp_fb))
                fprintf(stderr, "GSP transfer dst=%08X not shown (screen=%u shown=%08X) — shadow kept\n",
                        guest_dst, screen, gsp_fb_addr[screen][gsp_fb_shown[screen]]);
        } else {
            host_display_capture_transfer(dest, out_w, out_h, output_format);
        }
    }
    if (xp) {
        uint64_t xp_t1 = host_monotonic_ns();
        xfer_prof_report(xp_flush, xp_loop, xp_t1 - xp_t0,
                         input_w, input_h, out_w, out_h, flags);
    }
    /* Optional diagnostic overlay; use only an already discovered texture.
       Early transfers precede BFLIM loading. A failed RAM search per pixel
       would stall this SVC before PPF completion (8192 full RAM scans). */
    {
        static int logo_overlay;
        if (getenv("DQ8_DEBUG_LOGO_OVERLAY") && bflim_hdr_cache && !logo_overlay) {
            logo_overlay = 1;
            for (uint32_t y = 0; y < 32u; ++y)
                for (uint32_t x = 0; x < 256u; ++x) {
                    uint8_t r, g, b, a;
                    float u = (x + 0.5f) / 256.0f, v = (y + 0.5f) / 32.0f;
                    if (!pica_sample_bflim(u, v, &r, &g, &b, &a)) continue;
                    uint32_t dx = 40u + x, dy = 110u + y;
                    if (dx < HOST_TOP_W && dy < HOST_TOP_H) {
                        uint8_t *p = host_shadow_top + (dy * HOST_TOP_W + dx) * 4u;
                        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                        host_shadow_dirty = 1;
                    }
                }
        }
        gsp_write_screen_ppms();
    }
    host_display_present();
    gsp_enqueue_interrupt(4u); /* PPF: display transfer finished */
}
/* DQ8_TRACE_PX: report the FINAL presented pixel for every probe so the
   probe coordinates can be checked against the visible artifact line
   (landscape shadow: land X = memory row my = rt_h-1-y_raster,
   land Y = 239 - (x_raster >> 1) for the 512x400 -> 480->240 transfer). */
static void pica_px_present(void) {
    if (!pica_px_n) return;
    for (int i = 0; i < pica_px_n; i++) {
        int X = (int)pica_rt_h - 1 - pica_px_y[i];
        int Y = 239 - (pica_px_x[i] >> 1);
        const uint8_t *d;
        if (X < 0 || X >= 400 || Y < 0 || Y >= 240) continue;
        d = &host_shadow_top[((size_t)Y * 400u + (size_t)X) * 4u];
        fprintf(stderr, "PXS px=%d,%d land=%d,%d rgb=%u,%u,%u\n",
                pica_px_x[i], pica_px_y[i], X, Y, d[0], d[1], d[2]);
    }
}
/* Snapshot the presented screens for headless checks: the top screen and,
   because 3DS menus live there, the bottom screen too.  Opt-in via
   DQ8_CAPTURE_PPM=1 (audit 5.4: previously both PPMs were rewritten on
   EVERY display transfer with no gate).  One 5/s throttle inside this
   function covers every producer (transfer call site and vblank scan-out),
   each screen is serialized through a single buffered fwrite instead of
   per-pixel 3-byte writes, and DQ8_CAPTURE_PPM_DIR redirects the files so
   parallel runs cannot collide on the fixed /tmp names. */
static void gsp_write_screen_ppms_impl(void) {
    /* No pica_gl_flush_guest_rt() here: the guest-side producer flushes
       before snapshotting, and under DQ8_SPLIT_RENDER=1 this runs on the
       present worker, which must not touch the GL context. */
    if (!gsp_ppm_capture_enabled()) return;
    if (bench_no_snapshot) return; /* benchmark control only, see bench vars */
    ++bench_snapshots;
    {
        static uint64_t last_ppm_ns;
        uint64_t now = dq8_guest_now_ns();
        if (now - last_ppm_ns < 200000000ull) return;
        last_ppm_ns = now;
    }
    {   /* DQ8_DUMP_GUEST_DIR (diagnosis only, off by default): snapshot the
           guest color/depth buffers at capture time so GL and software runs
           can be compared in guest coordinates.  Skipped under
           DQ8_SPLIT_RENDER=1: this runs on the present worker, which never
           dereferences guest RAM (H1 contract §1). */
        const char *dd = getenv("DQ8_DUMP_GUEST_DIR");
        if (dd && dd[0] && !split_render_enabled()) {
            char dp[512];
            uint32_t mapped = 0;
            snprintf(dp, sizeof dp, "%s/guest_depth.raw", dd);
            FILE *zf = fopen(dp, "wb");
            if (zf) {
                if (pica_depth_addr &&
                    guest_memory_range(pica_depth_addr,
                                       (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp,
                                       &mapped))
                    fwrite(ram + mapped, 1,
                           (size_t)pica_rt_w * pica_rt_h * pica_depth_bpp, zf);
                fclose(zf);
            }
            snprintf(dp, sizeof dp, "%s/guest_color.raw", dd);
            FILE *cf = fopen(dp, "wb");
            if (cf) {
                mapped = 0;
                if (guest_memory_range(pica_rt_addr,
                                       (uint64_t)pica_rt_w * pica_rt_h * 4u, &mapped))
                    fwrite(ram + mapped, 1,
                           (size_t)pica_rt_w * pica_rt_h * 4u, cf);
                fclose(cf);
            }
            snprintf(dp, sizeof dp, "%s/guest_meta.txt", dd);
            FILE *mf = fopen(dp, "w");
            if (mf) {
                fprintf(mf, "rt %u %u bpp %u depth_bpp %u depth_bits %u bound %d "
                        "depth_addr %08X rt_addr %08X\n",
                        pica_rt_w, pica_rt_h, pica_color_bpp(pica_rt_format),
                        pica_depth_bpp, pica_depth_bits, pica_depth_bound,
                        (unsigned)pica_depth_addr, (unsigned)pica_rt_addr);
                fclose(mf);
            }
        }
    }
    const char *dir = getenv("DQ8_CAPTURE_PPM_DIR");
    char path[512];
    if (dir && dir[0])
        snprintf(path, sizeof path, "%s/dq8-screen.ppm", dir);
    else
        snprintf(path, sizeof path, "/tmp/dq8-screen.ppm");
    {
        FILE *f = fopen(path, "wb");
        if (f) {
            size_t n = (size_t)HOST_TOP_W * HOST_TOP_H;
            fprintf(f, "P6\n%u %u\n255\n", HOST_TOP_W, HOST_TOP_H);
            uint8_t *rgb = malloc(n * 3u);
            if (rgb) {
                for (size_t i = 0; i < n; ++i) {
                    rgb[i * 3u] = host_shadow_top[i * 4u];
                    rgb[i * 3u + 1u] = host_shadow_top[i * 4u + 1u];
                    rgb[i * 3u + 2u] = host_shadow_top[i * 4u + 2u];
                }
                fwrite(rgb, 3u, n, f);
                free(rgb);
            }
            fclose(f);
        }
    }
    if (dir && dir[0])
        snprintf(path, sizeof path, "%s/dq8-screen-bottom.ppm", dir);
    else
        snprintf(path, sizeof path, "/tmp/dq8-screen-bottom.ppm");
    {
        FILE *f = fopen(path, "wb");
        if (f) {
            size_t n = (size_t)HOST_BOT_W * HOST_BOT_H;
            fprintf(f, "P6\n%u %u\n255\n", HOST_BOT_W, HOST_BOT_H);
            uint8_t *rgb = malloc(n * 3u);
            if (rgb) {
                for (size_t i = 0; i < n; ++i) {
                    rgb[i * 3u] = host_shadow_bot[i * 4u];
                    rgb[i * 3u + 1u] = host_shadow_bot[i * 4u + 1u];
                    rgb[i * 3u + 2u] = host_shadow_bot[i * 4u + 2u];
                }
                fwrite(rgb, 3u, n, f);
                free(rgb);
            }
            fclose(f);
        }
    }
    pica_px_present();
}
/* Guest-thread wrapper: under DQ8_SPLIT_RENDER=1 the present worker owns
   the PPM snapshots (it owns the shadow buffers); guest-side producers
   only enqueue conversion jobs. */
static void gsp_write_screen_ppms(void) {
    if (split_render_enabled()) return;
    pica_gl_flush_guest_rt(); /* scan-out pixels must be current in guest RAM */
    gsp_write_screen_ppms_impl();
}
/* NATIVE_PRESENT_RUNTIME */
/* GSP scan-out: real hardware shows the buffer named by the FrameBufferUpdate
   in GSP shared memory (offset 0x200 + (2*tid+screen)*0x40), re-evaluated on
   every PDC0/PDC1 vblank (Azahar gsp_gpu.cpp SignalInterrupt -> SetBufferSwap).
   Presentation above only mirrors what the guest display-transfers; a screen
   drawn directly into the registered scan-out buffer (in-game dialogs rendered
   via PICA), or an active_fb switch that stops transfers targeting the shown
   buffer, is invisible if transfers alone drive the shadow. Capture the
   registered buffer on every top-screen vblank; when no valid framebuffer info
   is registered the transfer path stays authoritative.

   Slot model per Azahar GPU::SetBufferSwap: each shared-memory entry describes
   ONE slot — entry.active_fb says which slot (address_left1/2) the entry's
   address belongs to, entry.shown_fb says which slot the LCD displays. Keep
   both slot addresses persistently and show slot[shown_fb]. */
/* FRAMESTAT (t_86b8f1f9): renderer redundancy instrumentation, active with
   DQ8_FRAME_STATS=1.  Per PDC0 scan-out frame it records draw/triangle counts
   split GL vs software rasterizer (frame deltas) and an EXACT identity test of
   the displayed image against the previous frame (word compare of both shown
   framebuffer slots — capture-independent and mode-independent).  A frame
   with identical=1 is one where the whole frame's rasterization work produced
   a byte-equal displayed image: the redundancy a raster-elision (F1) lever
   would remove, measured for BOTH rendering modes (software mode = all draws
   counted in the sw column). */
static unsigned long long fs_frames, fs_identical, fs_ident_gl_draws,
    fs_ident_sw_draws, fs_ident_tris, fs_chg_words_total;
static unsigned long long fs_last_gl, fs_last_sw, fs_last_tg, fs_last_ts;
static uint8_t *fs_prev[2];
static uint32_t fs_prev_bytes[2], fs_prev_addr[2], fs_prev_stride[2],
    fs_prev_fmt[2];
static int fs_on = -1;

static void frame_stats_dump(void) {
    fprintf(stderr,
            "FRAMESTAT totals: frames=%llu identical=%llu identical_pct=%.1f%% "
            "draws_on_identical_frames: gl=%llu sw=%llu tris=%llu "
            "changed_words_total=%llu\n",
            fs_frames, fs_identical,
            fs_frames ? 100.0 * (double)fs_identical / (double)fs_frames : 0.0,
            fs_ident_gl_draws, fs_ident_sw_draws, fs_ident_tris,
            fs_chg_words_total);
}

static int frame_stats_on(void) {
    if (fs_on < 0) {
        const char *e = getenv("DQ8_FRAME_STATS");
        fs_on = (e && e[0] && e[0] != '0');
        if (fs_on) atexit(frame_stats_dump);
    }
    return fs_on;
}

static void frame_stats_sample(void) {
    int ok = 1, identical;
    uint32_t changed = 0;
    unsigned screen;
    unsigned long long gl, sw, tg, ts, dgl, dsw, dtg, dts;
    if (!frame_stats_on()) return;
    for (screen = 0; screen < 2u; ++screen) {
        uint32_t show = gsp_fb_shown[screen] & 1u;
        uint32_t addr = gsp_fb_addr[screen][show];
        uint32_t stride = gsp_fb_stride[screen];
        uint32_t fmt = gsp_fb_fmt[screen] & 7u;
        uint32_t bpp = gsp_transfer_bytes_per_pixel(fmt);
        uint32_t bytes, mapped, words, i;
        if (!gsp_fb_valid[screen] || !addr || !bpp || stride < 240u * bpp ||
            stride > 2400u * 4u) { ok = 0; continue; }
        bytes = stride * 240u;
        if (!guest_memory_range(addr, bytes, &mapped)) { ok = 0; continue; }
        /* Comparable whenever geometry matches the previous DISPLAYED image;
           the shown ADDRESS may alternate every vblank (guest double
           buffering) and must not invalidate the content comparison. */
        if (fs_prev[screen] && fs_prev_bytes[screen] == bytes &&
            fs_prev_stride[screen] == stride && fs_prev_fmt[screen] == fmt) {
            const uint32_t *a = (const uint32_t *)(const void *)(ram + mapped);
            const uint32_t *b = (const uint32_t *)(const void *)fs_prev[screen];
            words = bytes / 4u;
            for (i = 0; i < words; ++i)
                if (a[i] != b[i]) ++changed;
            memcpy(fs_prev[screen], ram + mapped, bytes);
        } else {
            if (!fs_prev[screen] || fs_prev_bytes[screen] < bytes) {
                free(fs_prev[screen]);
                fs_prev[screen] = (uint8_t *)malloc(bytes);
                fs_prev_bytes[screen] = fs_prev[screen] ? bytes : 0;
            }
            if (!fs_prev[screen]) { ok = 0; continue; }
            memcpy(fs_prev[screen], ram + mapped, bytes);
            fs_prev_addr[screen] = addr;
            fs_prev_stride[screen] = stride;
            fs_prev_fmt[screen] = fmt;
            ok = 0; /* first sight of this geometry is not an identical frame */
        }
    }
    gl = pica_gl_draws_ok; sw = pica_gl_draws_sw;
    tg = pica_tris_gl_total; ts = pica_tris_sw_total;
    dgl = gl - fs_last_gl; dsw = sw - fs_last_sw;
    dtg = tg - fs_last_tg; dts = ts - fs_last_ts;
    fs_last_gl = gl; fs_last_sw = sw; fs_last_tg = tg; fs_last_ts = ts;
    identical = (ok && changed == 0u);
    ++fs_frames;
    fs_chg_words_total += changed;
    if (identical) {
        ++fs_identical;
        fs_ident_gl_draws += dgl;
        fs_ident_sw_draws += dsw;
        fs_ident_tris += dtg + dts;
    }
    /* Cumulative totals ON EVERY LINE: scenario runs are wall-clock killed
       (SIGKILL), so atexit totals never print — the last line is the total. */
    /* GUESTPHASE (t_3de68379): direct measurement of the guest-interpretation
       share of the frame (DQ8_GUEST_TIME=1).  Printed on the same cadence as
       the FRAMESTAT totals so both numbers land on the same line cadence. */
    if (dq8_guest_time_enabled && ((fs_frames % 30u) == 1u || !identical)) {
        uint64_t dq8_gt_now = host_monotonic_ns();
        fprintf(stderr,
                "GUESTPHASE frames=%llu guest_ms=%.1f guest_ms_per_frame=%.2f "
                "guest_pct_wall=%.2f",
                fs_frames, (double)dq8_guest_time_ns / 1e6,
                fs_frames ? (double)dq8_guest_time_ns / 1e6 / (double)fs_frames : 0.0,
                (dq8_gt_now && dq8_gt_now > dq8_guest_time_t0)
                    ? 100.0 * (double)dq8_guest_time_ns /
                          (double)(dq8_gt_now - dq8_guest_time_t0)
                    : 0.0);
        fputc(10, stderr);
    }
    if ((fs_frames % 30u) == 1u || !identical)
        fprintf(stderr,
                "FRAMESTAT f=%llu gl=%llu sw=%llu tris_gl=%llu tris_sw=%llu "
                "same=%d chg=%u | cum: frames=%llu identical=%llu "
                "ident_pct=%.1f%% gl_draws=%llu sw_draws=%llu "
                "ident_gl=%llu ident_sw=%llu ident_tris=%llu "
                "stage_hits=%llu stage_misses=%llu stage_texels=%llu "
                "tex_uploads=%llu tex_bytes=%llu tex_hit=%llu tex_miss=%llu\n",
                fs_frames, dgl, dsw, dtg, dts, identical, changed,
                fs_frames, fs_identical,
                fs_frames ? 100.0 * (double)fs_identical / (double)fs_frames : 0.0,
                gl, sw, fs_ident_gl_draws, fs_ident_sw_draws, fs_ident_tris,
                pica_stage_hits, pica_stage_misses, pica_stage_texels_decoded,
                pgl_tex_uploads, pgl_tex_upload_bytes, pgl_tex_hits,
                pgl_tex_misses);
}

/* t_fa78ea1f diagnostic (DQ8_DARKLOG=1): per presented frame, report how much
   of each candidate image is black so a "menu missing" frame can be attributed
   to the render target (draw lost), the scan-out slot (transfer problem) or the
   presented shadow (capture).  dark = max(r,g,b) <= 16, sampled every 2nd px. */
static int dlog_on = -1;
static int dlog_enabled(void) {
    if (dlog_on < 0) {
        const char *e = getenv("DQ8_DARKLOG");
        dlog_on = (e && e[0] && e[0] != '0');
    }
    return dlog_on;
}
/* Dark share of a linear RGB8 guest region (scan-out slot layout). */
static double dlog_dark_linear(uint32_t addr, uint32_t stride, uint32_t h) {
    uint32_t mapped, x, y, n = 0, dark = 0;
    if (!addr || !guest_memory_range(addr, (uint64_t)stride * h, &mapped)) return -1.0;
    for (y = 0; y < h; y += 2u) {
        const uint8_t *p = ram + mapped + (uint64_t)y * stride;
        for (x = 0; x < stride; x += 6u) {
            uint8_t r = p[x+2], g = p[x+1], b = p[x];
            uint8_t m = r > g ? (r > b ? r : b) : (g > b ? g : b);
            ++n;
            if (m <= 16u) ++dark;
        }
    }
    return n ? 100.0 * (double)dark / (double)n : -1.0;
}
/* Dark share of the GPU render target (Morton-tiled RGBA8). */
static double dlog_dark_rt(void) {
    uint32_t x, y, n = 0, dark = 0;
    uint8_t r, g, b, a;
    if (!pica_rt_addr || !pica_rt_w || !pica_rt_h) return -1.0;
    for (y = 0; y < pica_rt_h; y += 8u) {
        for (x = 0; x < pica_rt_w; x += 8u) {
            if (!pica_get_pixel(pica_rt_addr, pica_rt_w, pica_rt_h,
                                pica_rt_format, x, y, &r, &g, &b, &a)) return -1.0;
            unsigned m = r > g ? (r > b ? r : b) : (g > b ? g : b);
            ++n;
            if (m <= 16u) ++dark;
        }
    }
    return n ? 100.0 * (double)dark / (double)n : -1.0;
}
static void pica_darklog_sample(void) {
    unsigned long long n, dark = 0;
    if (!dlog_enabled()) return;
    if (!gsp_fb_valid[0]) return;
    n = (unsigned long long)HOST_TOP_W * HOST_TOP_H;
    for (unsigned long long i = 0; i < n; i += 2u) {
        unsigned m = host_shadow_top[i * 4u];
        if (host_shadow_top[i * 4u + 1u] > m) m = host_shadow_top[i * 4u + 1u];
        if (host_shadow_top[i * 4u + 2u] > m) m = host_shadow_top[i * 4u + 2u];
        if (m <= 16u) ++dark;
    }
    fprintf(stderr,
            "DARKLOG shadow=%.1f rt=%08X %ux%u rt_dark=%.1f "
            "slot0=%08X d0=%.1f slot1=%08X d1=%.1f shown=%u stride=%u "
            "xfer=%08X->%08X %ux%u fmt=%u n=%u draws=%llu\n",
            100.0 * (double)dark / (double)(n / 2u),
            pica_rt_addr, pica_rt_w, pica_rt_h, dlog_dark_rt(),
            gsp_fb_addr[0][0], dlog_dark_linear(gsp_fb_addr[0][0], gsp_fb_stride[0], 240u),
            gsp_fb_addr[0][1], dlog_dark_linear(gsp_fb_addr[0][1], gsp_fb_stride[0], 240u),
            gsp_fb_shown[0] & 1u, gsp_fb_stride[0],
            dlog_xsrc, dlog_xdst, dlog_xw, dlog_xh, dlog_xfmt, dlog_xn,
            (unsigned long long)pica_draw_count);
}

static void gsp_scanout_capture(void) {
    pica_gl_flush_guest_rt(); /* scan-out pixels must be current in guest RAM */
    if (!gsp_shared_base) return;
    for (unsigned screen = 0; screen < 2u; ++screen) {
        uint32_t base = gsp_shared_base + 0x200u + (2u * gsp_thread_id + screen) * 0x40u;
        uint32_t mapped;
        if (!guest_memory_range(base, 0x40u, &mapped)) continue;
        const uint8_t *p = ram + mapped;
        uint8_t index = p[0], dirty = p[1];
        const uint32_t *info = (const uint32_t *)(const void *)(p + 4u) + (index & 1u) * 7u;
        uint32_t active_fb = info[0] & 1u;
        uint32_t addr = info[1]; /* address_left */
        uint32_t stride = info[3];
        uint32_t fmt = info[4];
        uint32_t shown_fb = info[5] & 1u;
        /* Explicit update semantics (A08 fix).  A dirty entry is a published
           framebuffer update; applying it must never depend on the NUMERIC
           value of shown_fb: slot 0 is a valid slot index, not "no value"
           (Azahar gpu.cpp assigns shown_fb unconditionally).  The entry's
           address fields carry their own validity signal (addr != 0) and
           update only the slot named by active_fb; the shown-slot update is
           independent of them.  The dirty byte is left as the guest published
           it (level semantics: writer discipline is not observable here, and
           re-applying one entry is idempotent). */
        if (dirty) {
            if (screen == 0u) tempo_swap_seen(); /* guest published a top-screen frame */
            if (addr) {
                gsp_fb_addr[screen][active_fb] = addr;
                gsp_fb_fmt[screen] = fmt;
                gsp_fb_stride[screen] = stride;
                gsp_fb_valid[screen] = 1;
            }
            gsp_fb_shown[screen] = shown_fb;
        }
        if (!gsp_fb_valid[screen]) continue;
        uint32_t show = gsp_fb_shown[screen];
        addr = gsp_fb_addr[screen][show];
        stride = gsp_fb_stride[screen];
        fmt = gsp_fb_fmt[screen] & 7u;
        uint32_t bpp = gsp_transfer_bytes_per_pixel(fmt);
        if (!addr || !bpp || stride < 240u * bpp || stride > 2400u * 4u) continue;
        uint32_t w = stride / bpp, h = screen ? 320u : 400u;
        uint32_t mapped_fb;
        if (!guest_memory_range(addr, (uint64_t)stride * h, &mapped_fb)) continue;
        if (dq8_env_on("DQ8_TRACE_GSP_FB", &dq8_env_trace_gsp_fb) &&
            (addr != gsp_fb_addr[screen][!show] || dirty))
            fprintf(stderr, "GSP scan-out screen=%u addr=%08X fmt=%u stride=%u "
                    "slot=%u shown=%u dirty=%u\n",
                    screen, addr, fmt, stride, active_fb, show, dirty);
        host_display_capture_transfer(mapped_fb, w, h, fmt);
    }
    /* IDLE THROTTLE: feed the scene-identity detector with the just-captured
       shown-slot regions (both screens, guest RAM, capture-independent). */
    idle_scanout_sample();
    frame_stats_sample(); /* FRAMESTAT redundancy instrumentation (opt-in) */
    pica_darklog_sample(); /* t_fa78ea1f menu-flicker diagnosis (opt-in) */
    /* Refresh the snapshots: gsp_write_screen_ppms is opt-in
       (DQ8_CAPTURE_PPM=1) and carries the shared 5/s throttle for every
       producer, so this call site needs no throttle of its own. */
    gsp_write_screen_ppms();
}
static void gsp_enqueue_interrupt(uint8_t event) {
    if (event < 8u) ++bench_irq_counts[event]; /* measurement harness only */
    if (event == 2u) { PF_ENTER(PF_PRESENT); gsp_scanout_capture(); PF_EXIT(); pf_frame_end(); ++pqt_frames; tempo_frame_seen(); } /* PDC0: evaluate scan-out buffer */    if (!gsp_shared_base) return;
    uint32_t q = gsp_shared_base + gsp_thread_id * 0x40u;
    uint8_t cur = mem8(q), count = mem8(q + 1u);
    if (count >= ((event == 2u || event == 3u) ? 0x20u : 0x34u)) { tempo_gsp_drop_seen(); return; }
    put8(q + 0x0Cu + ((cur + count) % 0x34u), event);
    put8(q + 1u, (uint8_t)(count + 1u));
    if (gsp_relay_registered && gsp_relay_event < next_event_handle && event_live[gsp_relay_event]) {
        event_signaled[gsp_relay_event] = 1u;
        guest_notify_handle(gsp_relay_event);
    }
}
static void guest_mem_check(uint32_t a, unsigned n) {
    uint32_t mapped;
    if (!guest_memory_range(a, n, &mapped)) {
        if (active_guest_regs && active_guest_regs[4] < GUEST_RAM_SIZE - 0x1600u) {
            uint32_t base = active_guest_regs[4] + 0x1580u;
            fprintf(stderr, "fault context object=%08X words@+1580=", active_guest_regs[4]);
            for (unsigned i = 0; i < 24u; ++i)
                fprintf(stderr, "%08X%s", *(uint32_t *)(void *)(ram + base + i * 4u), i == 23u ? "\\n" : "/");
        }
        if (active_guest_regs && active_guest_regs[13] < GUEST_RAM_SIZE - 0x100u) {
            uint32_t base = active_guest_regs[13];
            fprintf(stderr, "fault stack=%08X words=", base);
            for (unsigned i = 0; i < 48u; ++i)
                fprintf(stderr, "%08X%s", *(uint32_t *)(void *)(ram + base + i * 4u), i == 47u ? "\\n" : "/");
        }
        if (active_guest_regs) {
            /* Escape-hunting round 6: the faulting tree/list op (0x0014218C)
               reads a corrupt link; dump the node it walked to (r1), the
               +0x8 chain from the head (r0, the walk at 0x00131B4C), the bad
               link target (r3) and the sibling objects (r2/r5/r6) so the
               corrupting writer is identifiable from the log alone. */
            uint32_t node = active_guest_regs[1];
            if (node && node < GUEST_RAM_SIZE - 0x40u) {
                fprintf(stderr, "fault node@%08X words=", node);
                for (unsigned i = 0; i < 16u; ++i)
                    fprintf(stderr, "%08X%s", *(uint32_t *)(void *)(ram + node + i * 4u), i == 15u ? "\\n" : "/");
            }
            uint32_t cur = active_guest_regs[0];
            fprintf(stderr, "fault chain head@%08X =", cur);
            for (unsigned i = 0; i < 8u && cur && cur < GUEST_RAM_SIZE - 0x10u; ++i) {
                uint32_t nxt = *(uint32_t *)(void *)(ram + cur + 8u);
                fprintf(stderr, " [%08X->%08X]", cur, nxt);
                cur = nxt;
            }
            fprintf(stderr, "\\n");
            for (unsigned k = 2u; k <= 6u; ++k) {
                if (k == 4u) continue; /* r4 already dumped as context object */
                uint32_t o = active_guest_regs[k];
                if (o && o < GUEST_RAM_SIZE - 0x20u) {
                    fprintf(stderr, "fault r%u@%08X words=", k, o);
                    for (unsigned i = 0; i < 8u; ++i)
                        fprintf(stderr, "%08X%s", *(uint32_t *)(void *)(ram + o + i * 4u), i == 7u ? "\\n" : "/");
                }
            }
        }
        fprintf(stderr, "guest memory fault: pc=0x%08X address=0x%08X width=%u r0-r7=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X lr=%08X sp=%08X\n",
                current_guest_pc, a, n,
                active_guest_regs ? active_guest_regs[0] : 0u,
                active_guest_regs ? active_guest_regs[1] : 0u,
                active_guest_regs ? active_guest_regs[2] : 0u,
                active_guest_regs ? active_guest_regs[3] : 0u,
                active_guest_regs ? active_guest_regs[4] : 0u,
                active_guest_regs ? active_guest_regs[5] : 0u,
                active_guest_regs ? active_guest_regs[6] : 0u,
                active_guest_regs ? active_guest_regs[7] : 0u,
                active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u);
        exit(16);
    }
}
static void trace_hid_read(uint32_t a, uint32_t v) {
    if (!trace_hid_read_base || a < trace_hid_read_base || a >= trace_hid_read_end) return;
    if (trace_hid_runtime_ctx) return; /* runtime helper keeping its own state, not a guest poll */
    uint32_t pc = current_guest_pc;
    struct trace_hid_pc_state *s = NULL;
    for (unsigned i = 0; i < trace_hid_pc_count; ++i)
        if (trace_hid_pcs[i].pc == pc) { s = &trace_hid_pcs[i]; break; }
    if (!s) {
        if (trace_hid_pc_count >= 64u) return;
        s = &trace_hid_pcs[trace_hid_pc_count++];
        s->pc = pc; s->hits = 0; s->last_val = v; s->last_sec = 0;
    }
    s->hits++;
    uint64_t sec = (host_monotonic_ns() - trace_hid_t0_ns) / 1000000000ull;
    /* First hit of a PC always logs (proves WHO reads), then only value
     * changes (at most once per second) or a 10 s heartbeat (WHEN reads
     * continue) so a hot poll loop cannot flood the log. */
    if (s->hits == 1u || sec >= s->last_sec + 10u || (v != s->last_val && sec != s->last_sec)) {
        unsigned thx = (current_guest_thread_id >= 0x50000u && current_guest_thread_id < 0x50100u)
                           ? (unsigned)(current_guest_thread_id - 0x50000u) : 99u;
        fprintf(stderr, "HIDREAD t=%llu th=%u pc=%08X addr=%08X off=%03X val=%08X lr=%08X sp=%08X hits=%u\n",
                (unsigned long long)sec, thx, pc, a, a - trace_hid_read_base, v,
                active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u, s->hits);
        s->last_val = v; s->last_sec = sec;
    }
}
/* Guest ARMv6 LDR/STR/LDRH/STRH are legal on unaligned addresses, so the
   memory helpers must not use aligned typed accesses: `*(uint32_t *)` on a
   guest address is alignment UB and lets the compiler (now the default -O2
   runtime TU) assume 4-byte alignment.  memcpy compiles to exactly one
   unaligned mov on x86-64, so the generated code is unchanged; the linked
   binary is byte-identical before/after this fix (reports/UB_UNALIGNED_MEM.md).
   There is no mem64 here: guest LDRD is emitted as two mem32 accesses. */
static uint32_t mem32(uint32_t a) { guest_mem_check(a, 4); uint32_t v; memcpy(&v, ram + guest_memory_address(a), sizeof(v)); if (trace_hid_reads_enabled) trace_hid_read(a, v); return v; }
static uint32_t mem8(uint32_t a) { guest_mem_check(a, 1); uint32_t v = ram[guest_memory_address(a)]; if (trace_hid_reads_enabled) trace_hid_read(a, v); return v; }
static uint32_t mem16(uint32_t a) { guest_mem_check(a, 2); uint16_t w; memcpy(&w, ram + guest_memory_address(a), sizeof(w)); if (trace_hid_reads_enabled) trace_hid_read(a, (uint32_t)w); return w; }
static uint32_t read_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t read_le64(const uint8_t *p) { return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32); }
/* ARMv6 store -> single-core exclusive monitor (audit R2-02).  Any ordinary
   store that overlaps the reserved range drops the reservation, so a later
   STREX fails instead of silently overwriting the other writer's value.
   Reservations also never survive a guest context switch (see
   schedule_guest_thread), matching the 3DS kernel's clear-on-switch
   behavior.  Ranges are widened to 64 bits so a store at the top of the
   address space cannot alias by wrap-around. */
static void exclusive_check_store(uint32_t a, uint32_t size) {
    if (exclusive_addr == 0xFFFFFFFFu) return;
    uint64_t end = (uint64_t)a + size;
    uint64_t xend = (uint64_t)exclusive_addr + exclusive_size;
    if ((uint64_t)a < xend && end > (uint64_t)exclusive_addr)
        exclusive_addr = 0xFFFFFFFFu;
}
static void put32(uint32_t a, uint32_t v) {
    guest_mem_check(a, 4);
    exclusive_check_store(a, 4u);
    uint32_t mapped = guest_memory_address(a);
    if (trace_write_hit(a) >= 0 && trace_guest_write_hits++ < 200000u) {
        uint32_t old; memcpy(&old, ram + mapped, sizeof(old));
        fprintf(stderr, "guest write addr=%08X pc=%08X old=%08X new=%08X lr=%08X sp=%08X r0=%08X r1=%08X r4=%08X\n",
                a, current_guest_pc, old, v, active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u, active_guest_regs ? active_guest_regs[0] : 0u,
                active_guest_regs ? active_guest_regs[1] : 0u, active_guest_regs ? active_guest_regs[4] : 0u);
    }
    memcpy(ram + mapped, &v, sizeof(v));
    pica_gl_note_guest_write(ram + mapped, 4u);
}
static void put8(uint32_t a, uint32_t v) {
    guest_mem_check(a, 1);
    exclusive_check_store(a, 1u);
    if (trace_write_hit(a) >= 0 && trace_guest_write_hits++ < 200000u)
        fprintf(stderr, "guest write8 addr=%08X pc=%08X old=%02X new=%02X lr=%08X sp=%08X\n", a, current_guest_pc,
                mem8(a), (uint8_t)v, active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u);
    ram[guest_memory_address(a)] = (uint8_t)v;
    pica_gl_note_guest_write(ram + guest_memory_address(a), 1u);
}
static void put16(uint32_t a, uint32_t v) {
    guest_mem_check(a, 2);
    exclusive_check_store(a, 2u);
    if (trace_write_hit(a) >= 0 && trace_guest_write_hits++ < 200000u)
        fprintf(stderr, "guest write16 addr=%08X pc=%08X old=%04X new=%04X lr=%08X sp=%08X\n", a, current_guest_pc,
                mem16(a), (uint16_t)v, active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u);
    uint16_t x = (uint16_t)v; memcpy(ram + guest_memory_address(a), &x, sizeof(x));
    pica_gl_note_guest_write(ram + guest_memory_address(a), 2u);
}
static uint32_t arm_shift(uint32_t value, unsigned kind, unsigned amount) {
    if (!amount) return value;
    switch (kind) {
    case 0: return amount < 32 ? value << amount : 0;
    case 1: return amount < 32 ? value >> amount : 0;
    case 2: return amount < 32 ? (uint32_t)((int32_t)value >> amount) : (uint32_t)((int32_t)value >> 31);
    default: amount &= 31; return amount ? (value >> amount) | (value << (32 - amount)) : value;
    }
}

static uint32_t uqsub8(uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned x = (a >> (i * 8)) & 255u;
        unsigned y = (b >> (i * 8)) & 255u;
        out |= (x > y ? x - y : 0u) << (i * 8);
    }
    return out;
}
static uint32_t qsub8(uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 4; ++i) {
        int x = (int8_t)(a >> (i * 8));
        int y = (int8_t)(b >> (i * 8));
        int d = x - y;
        if (d > INT8_MAX) d = INT8_MAX;
        if (d < INT8_MIN) d = INT8_MIN;
        out |= (uint32_t)(uint8_t)(int8_t)d << (i * 8);
    }
    return out;
}
static uint32_t qsub16(uint32_t a, uint32_t b) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 2; ++i) {
        int x = (int16_t)(a >> (i * 16));
        int y = (int16_t)(b >> (i * 16));
        int d = x - y;
        if (d > INT16_MAX) d = INT16_MAX;
        if (d < INT16_MIN) d = INT16_MIN;
        out |= (uint32_t)(uint16_t)(int16_t)d << (i * 16);
    }
    return out;
}

/* ARMv6 parallel (SIMD) arithmetic over 8- or 16-bit lanes.  DQ8's movie
   decoder blend loops (entry 0x00195C20) are built from this family plus
   SEL, so the lifter must model it rather than treating the shared single
   data transfer encoding space as an unknown opcode.
   bits: lane width (8 or 16).
   op:   0 add, 1 subtract, 2 ASX (low = a0+b1, high = a1-b0),
         3 SAX (low = a0-b1, high = a1+b0).
   mode: 0 plain (also writes GE[3:0] through ge_out, consumed by SEL),
         1 saturating signed, 2 saturating unsigned, 3 halving signed,
         4 halving unsigned. */
static uint32_t par_op(uint32_t a, uint32_t b, unsigned bits, unsigned op, unsigned mode, int *ge_out) {
    unsigned lanes = 32u / bits;
    uint32_t mask = (1u << bits) - 1u;
    unsigned sign_shift = 32u - bits;
    uint32_t out = 0;
    unsigned ge = 0;
    for (unsigned i = 0; i < lanes; ++i) {
        uint32_t x = (a >> (i * bits)) & mask;
        unsigned j = i;
        unsigned lane_op = op & 1u;
        if (op >= 2u) {
            /* ASX/SAX exchange the Rm halfwords and alternate the operation:
               ASX = Rn_lo - Rm_hi, Rn_hi + Rm_lo;
               SAX = Rn_lo + Rm_hi, Rn_hi - Rm_lo. */
            j = lanes - 1u - i;
            lane_op = ((op == 2u) ? i != 0u : i == 0u) ? 0u : 1u;
        }
        uint32_t y = (b >> (j * bits)) & mask;
        int32_t sx = (int32_t)(x << sign_shift) >> sign_shift;
        int32_t sy = (int32_t)(y << sign_shift) >> sign_shift;
        uint32_t r;
        int lane_ge = 0;
        if (mode == 0u) {
            if (lane_op == 0u) {
                uint32_t s = x + y;
                r = s & mask;
                lane_ge = s > mask;
            } else {
                int32_t d = (int32_t)x - (int32_t)y;
                r = (uint32_t)d & mask;
                lane_ge = d >= 0;
            }
        } else if (mode == 1u) {
            int32_t s = lane_op == 0u ? sx + sy : sx - sy;
            int32_t hi = (int32_t)(mask >> 1);
            int32_t lo = -hi - 1;
            if (s > hi) s = hi;
            if (s < lo) s = lo;
            r = (uint32_t)s & mask;
        } else if (mode == 2u) {
            if (lane_op == 0u) {
                uint32_t s = x + y;
                r = s > mask ? mask : s;
            } else {
                r = x > y ? x - y : 0u;
            }
        } else if (mode == 3u) {
            int32_t s = lane_op == 0u ? sx + sy : sx - sy;
            r = (uint32_t)(s >> 1) & mask;
        } else if (lane_op == 0u) {
            r = ((x + y) >> 1) & mask;
        } else {
            int32_t d = (int32_t)x - (int32_t)y;
            r = (uint32_t)(d >> 1) & mask;
        }
        out |= (r & mask) << (i * bits);
        /* A 16-bit lane drives two GE bits, an 8-bit lane drives one. */
        if (lane_ge) ge |= bits == 8u ? 1u << i : 3u << (i * 2u);
    }
    if (mode == 0u && ge_out) *ge_out = (int)(ge & 0xFu);
    return out;
}
/* SEL: per byte lane take Rn when GE[i] is set, otherwise Rm. */
static uint32_t par_sel(uint32_t rn, uint32_t rm, int ge) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 4u; ++i)
        out |= ((((uint32_t)ge >> i) & 1u) ? rn : rm) & (0xFFu << (i * 8));
    return out;
}
static uint32_t arm_usat(int32_t value, unsigned sat) {
    uint64_t maximum = sat ? ((UINT64_C(1) << sat) - 1u) : 0u;
    if (value < 0) return 0u;
    return (uint64_t)value > maximum ? (uint32_t)maximum : (uint32_t)value;
}
static uint32_t arm_ssat(int32_t value, unsigned sat) {
    unsigned width = sat + 1u;
    int64_t minimum = -(INT64_C(1) << (width - 1u));
    int64_t maximum = (INT64_C(1) << (width - 1u)) - 1u;
    if ((int64_t)value < minimum) return (uint32_t)(int32_t)minimum;
    if ((int64_t)value > maximum) return (uint32_t)(int32_t)maximum;
    return (uint32_t)value;
}
static uint32_t arm_usat16(uint32_t value, unsigned sat) {
    uint32_t out = 0;
    for (unsigned lane = 0; lane < 2; ++lane) {
        int32_t x = (int16_t)(value >> (lane * 16u));
        out |= (arm_usat(x, sat) & 0xFFFFu) << (lane * 16u);
    }
    return out;
}
static uint32_t arm_ssat16(uint32_t value, unsigned sat) {
    uint32_t out = 0;
    for (unsigned lane = 0; lane < 2; ++lane) {
        int32_t x = (int16_t)(value >> (lane * 16u));
        out |= (arm_ssat(x, sat) & 0xFFFFu) << (lane * 16u);
    }
    return out;
}
#define GUEST_HOST_POLL_NS 16666667ull
static uint64_t guest_next_host_poll_ns;

/* IDLE THROTTLE: while armed the GSP vblank deadline is spaced by the
   auto-tuned idle period instead of one host poll, so the guest parks
   between frames.  The host pump (HID/window events) keeps its 16.67 ms
   cadence in both modes: it is the input-latency budget. */
static uint64_t idle_vblank_period_ns(void) {
    if (split_render_enabled()) return GSP_VBLANK_NS; /* F5: fixed 60 Hz chain */
    return idle_armed ? idle_period_ns : GUEST_HOST_POLL_NS;
}

/* PACE (t_3463e625): the guest-visible vblank clock is a property of the
   emulated hardware, not of how long our own frame took.  The inline path used
   to re-anchor the vblank deadline at `now` on every guest wait, so the guest's
   frame clock ran at 1/(16.67 ms + work-per-frame) instead of 60 Hz - a slow
   renderer slowed GAME time, not just the frame rate (measured 0.70x wall time
   on the savestate scene, worse on the operator's machine).  The chain below is
   deadline-based: each period is added to the previous deadline, so time the
   guest spent elsewhere is caught up (bounded by the cap) instead of given away.
   DQ8_VBLANK_CHAIN=0 restores the re-anchored behaviour for A/B. */
static uint64_t guest_vblank_period_ns(void) {
    static uint64_t period;
    if (!period) {
        const char *e = getenv("DQ8_VBLANK_HZ");
        unsigned long hz = (e && e[0]) ? strtoul(e, NULL, 10) : 0ul;
        period = hz ? (1000000000ull / hz) : GUEST_HOST_POLL_NS;
        if (!period) period = GUEST_HOST_POLL_NS;
    }
    return period;
}
static int vblank_chain_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("DQ8_VBLANK_CHAIN");
        enabled = (e && e[0] == '0' && !e[1]) ? 0 : 1;
    }
    return enabled;
}
/* How many elapsed vblank periods one service call may deliver.  The bound
   limits the burst cost; the value matters because a period that is NOT
   delivered is guest frame time that never comes back (the resync below).
   One delivered period costs a scan-out, and the relay queue holds 32 entries
   (PDC0+PDC1 per period), so 16 is the largest safe value.
   DQ8_VBLANK_CATCHUP overrides it for measurement. */
static unsigned vblank_catchup_cap(void) {
    static unsigned cap;
    if (!cap) {
        const char *e = getenv("DQ8_VBLANK_CATCHUP");
        unsigned long v = (e && e[0]) ? strtoul(e, NULL, 10) : 16ul;
        if (v < 1ul) v = 1ul;
        if (v > 16ul) v = 16ul;
        cap = (unsigned)v;
    }
    return cap;
}
static void guest_vblank_service(uint64_t now) {
    if (split_render_enabled()) { gsp_vblank_service(now); return; }
    /* VBLANK WALLCLOCK (t_d1be62fc): the 60 Hz chain lives on the pacer
       thread; this call site only delivers the periods it has accounted. */
    if (vblank_wallclock_enabled()) { gsp_vbl_deliver_owed(); return; }
    if (!vblank_chain_enabled()) {
        if (gsp_relay_registered && (!gsp_next_vblank_ns || now >= gsp_next_vblank_ns)) {
            gsp_next_vblank_ns = now + idle_vblank_period_ns();
            gsp_enqueue_interrupt(2u); /* PDC0: top-screen VBlank */
            gsp_enqueue_interrupt(3u); /* PDC1: bottom-screen VBlank */
        }
        return;
    }
    /* Idle: one throttled delivery per tuned period (the shipped idle-throttle
       behaviour, which exists to stop a static menu burning a core).  Active:
       the hardware 60 Hz chain with bounded catch-up.
       DQ8_VBLANK_HZ overrides the active period for measurement only: it is
       how the guest's frame-lock ratio (logic ticks per vblank) is identified
       without guessing. */
    gsp_vblank_service_period(now, idle_armed ? idle_period_ns : guest_vblank_period_ns(),
                              idle_armed ? 1u : vblank_catchup_cap());
}

/* Service the externally paced sources while a main-context syscall waits.
   HID input is still sampled no slower than one actual ring period; GSP keeps
   its own vblank deadline, and schedule_guest_thread() services DSP quanta. */
static void guest_service_host_deadlines(uint64_t now) {
    guest_vblank_service(now);
    if (!guest_next_host_poll_ns || now >= guest_next_host_poll_ns) {
        ++idle_pump_count;
        idle_service_host_wake(now);
        host_display_pump_events();
        host_display_present();
        guest_next_host_poll_ns = now + GUEST_HOST_POLL_NS;
    }
}

/* Earliest condition that can make a currently idle main waiter runnable or
   require host-side servicing.  The caller supplies its own (possibly absent)
   timeout; blocked worker deadlines, vblank, DSP and HID/window polling remain
   independent sources. */
static uint64_t guest_next_wait_deadline(uint64_t caller_deadline) {
    uint64_t wake = caller_deadline ? caller_deadline : UINT64_MAX;
    if (guest_next_host_poll_ns && guest_next_host_poll_ns < wake)
        wake = guest_next_host_poll_ns;
    if (gsp_relay_registered && gsp_next_vblank_ns && gsp_next_vblank_ns < wake)
        wake = gsp_next_vblank_ns;
    if (dsp_audio_on && dsp_next_frame_ns && dsp_next_frame_ns < wake)
        wake = dsp_next_frame_ns;
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        const GuestThread *thread = &guest_threads[i];
        if (thread->active && thread->blocked && thread->wake_ns && thread->wake_ns < wake)
            wake = thread->wake_ns;
    }
    return wake;
}

/* This helper is called only after the caller has serviced a scheduler slice
   and established that no peer is runnable.  Never park runnable guest work. */
static void guest_wait_until_deadline(uint64_t caller_deadline) {
    if (guest_any_runnable_thread()) return;
    uint64_t now = dq8_guest_now_ns();
    uint64_t wake = guest_next_wait_deadline(caller_deadline);
    if (dq8_det_mode) {
        /* DETERMINISTIC (t_115d297c): sleeping on the host cannot advance a
           virtual clock, so hand out the virtual time up to the earliest
           deadline instead.  The jump is a pure function of the scheduler
           state, so the same state always yields the same next event.  An
           unbounded wait (no deadline at all) advances one host-poll period
           so an external producer still gets a chance to run. */
        dq8_det_advance_to(wake == UINT64_MAX ? now + GUEST_HOST_POLL_NS : wake);
        return;
    }
    if (wake != UINT64_MAX && wake > now) {
        /* FRAMEPROF: a nap bounded by the GSP vblank deadline is the frame's
           vblank parking (present_ms); any other park is idle_ms. */
        int pf_vblank_park = (gsp_relay_registered && gsp_next_vblank_ns &&
                              wake == gsp_next_vblank_ns);
        PF_ENTER(pf_vblank_park ? PF_PRESENT : PF_IDLE);
        guest_host_sleep_ns(wake - now);
        PF_EXIT();
    }
}

/* Q1 (t_039e900a) A/B gate for the runnable-aware nap.  When a main-context
   wait serviced its peers and found none runnable it has two choices:

     * merged policy (DQ8_RUNNABLE_NAP unset or non-"0"): nap only to the
       earliest main/worker/vblank/DSP/HID-poll deadline (guest_wait_until_deadline),
       so a host sleep never covers a timeout that some peer could still meet;
     * control policy (DQ8_RUNNABLE_NAP=0): the pre-da1d8ff fixed-pacing nap --
       one short host sleep per serviced round regardless of the deadline.

   Read once per process (dq8_env_on() style) so the hot wait loops do not pay
   a getenv per iteration.  The gate defaults to the merged policy, so an
   environment without the variable executes exactly the previous statements. */
static int runnable_nap_gate = -1;
static int runnable_nap(void) {
    if (runnable_nap_gate < 0) {
        const char *e = getenv("DQ8_RUNNABLE_NAP");
        runnable_nap_gate = !(e && e[0] == '0' && e[1] == '\0');
    }
    return runnable_nap_gate;
}
/* Legacy fixed-pacing nap: sleep `quantum` (clamped to the caller deadline and
   never negative).  Used only when DQ8_RUNNABLE_NAP=0. */
static void guest_pacing_nap(uint64_t caller_deadline, uint64_t quantum) {
    uint64_t now = dq8_guest_now_ns();
    uint64_t wake = now + quantum;
    if (caller_deadline && caller_deadline < wake) wake = caller_deadline;
    if (wake > now) guest_host_sleep_ns(wake - now);
}

/* Main-context sleep: execute bounded round-robin peer slices until the
   caller's deadline.  The host naps only when all peers are blocked, and then
   only to the earliest main/worker/service deadline.  A negative sleep keeps
   the established no-wake semantics: it runs ready peers and returns once
   there is no runnable peer. */
static void main_context_sleep(int64_t duration) {
    uint64_t start = dq8_guest_now_ns();
    uint64_t deadline = duration > 0 ? start + (uint64_t)duration : 0u;
    for (;;) {
        uint64_t now = dq8_guest_now_ns();
        if (deadline && now >= deadline) return;
        schedule_guest_thread();
        guest_service_host_deadlines(dq8_guest_now_ns());
        if (!deadline && !guest_any_runnable_thread()) return;
        if (guest_any_runnable_thread()) continue;
        if (!runnable_nap()) { guest_pacing_nap(deadline, 1000000ull); continue; }
        guest_wait_until_deadline(deadline);
    }
}
static void svc_sleep(uint64_t ns) {
    if (movie_trace()) {
        ++movie_trace_sleeps;
        if ((int64_t)ns > 0) {
            movie_trace_sleep_ns += ns;
            if (ns > movie_trace_sleep_max_ns) movie_trace_sleep_max_ns = ns;
        }
        movie_trace_event("sleep thread=%08X pc=%08X ns=%lld",
                          current_guest_thread_id, current_guest_pc, (long long)ns);
    }
    /* 3DS SleepThread takes a signed 64-bit nanosecond duration.  Zero is
       "yield if another thread is ready"; a positive value is a real wake
       deadline; -1 (any negative, as azahar's WakeAfterDelay treats it) means
       no wake event, i.e. an indefinite park. */
    int64_t duration = (int64_t)ns;
    tempo_sleep_seen(duration > 0 ? (uint64_t)duration : 0u);
    if (thread_execution_depth) {
        thread_yield_requested = 1;
        if (duration == 0) {
            /* Pure yield: no park, no deadline, nothing to wake. */
            thread_sleep_requested_ns = 0;
            thread_wake_result_requested = 0;
            return;
        }
        ++perf_worker_sleeps;
        for (unsigned i = 0; i < guest_thread_count; ++i)
            if (guest_threads[i].guest_thread_id == current_guest_thread_id)
                ++guest_threads[i].sleep_calls;
        thread_blocked_requested = 1;
        thread_wait_handle_requested = 0;
        /* A non-positive duration has no deadline: the scheduler records
           wake_ns = 0 and the thread stays blocked. */
        thread_sleep_requested_ns = duration > 0 ? idle_stretch_wait((uint64_t)duration) : 0u;
        thread_wake_result_requested = 0;
        return; /* the dispatch returns to the scheduler, which owns the park */
    }
    if (duration == 0) {
        /* No deadline: one reschedule opportunity, no sleep, no busy poll. */
        guest_host_yield();
        return;
    }
    ++perf_main_sleeps;
    perf_main_sleep_ns += duration > 0 ? (uint64_t)duration : 0u;
    main_context_sleep(duration);
}
static int arm_shift_carry(uint32_t value, unsigned kind, unsigned amount, int carry) {
    if (!amount) return kind == 3 ? value & 1u : carry;
    switch (kind) {
    case 0: return amount < 32 ? (value >> (32 - amount)) & 1u : amount == 32 ? value & 1u : 0;
    case 1: return amount < 32 ? (value >> (amount - 1)) & 1u : amount == 32 ? value >> 31 : 0;
    case 2: return amount < 32 ? (value >> (amount - 1)) & 1u : value >> 31;
    default: { unsigned rot = amount & 31; return rot ? (value >> (rot - 1)) & 1u : value >> 31; }
    }
}
static uint32_t arm_shift_imm(uint32_t value, unsigned kind, unsigned amount, int carry) {
    if (amount) return arm_shift(value, kind, amount);
    switch (kind) {
    case 0: return value;
    case 1: return 0;
    case 2: return (uint32_t)((int32_t)value >> 31);
    default: return ((uint32_t)carry << 31) | (value >> 1);
    }
}
static int arm_shift_imm_carry(uint32_t value, unsigned kind, unsigned amount, int carry) {
    /* Carry-out for the immediate shift forms.  Unlike the
       register-controlled arm_shift_carry, an encoded amount of 0 here means
       LSR #32 / ASR #32 (carry = bit 31 of the source) or RRX for ROR; only
       LSL #0 leaves the incoming carry in place.  Immediate amounts are in
       0..31, so no shift here ever reaches the 32-bit width. */
    switch (kind) {
    case 0: return amount ? (value >> (32 - amount)) & 1u : carry;
    case 1: case 2: return amount ? (value >> (amount - 1)) & 1u : value >> 31;
    default: { unsigned rot = amount & 31u; return rot ? (value >> (rot - 1)) & 1u : value & 1u; }
    }
}
static uint32_t branch_indirect(uint32_t target) {
    static unsigned trace_branch_count;
    if (dq8_env_on("DQ8_TRACE_BRANCH", &dq8_env_trace_branch) && target >= 0x00167000u && target < 0x00168000u && trace_branch_count++ < 128u)
        fprintf(stderr, "guest BX pc=%08X target=%08X lr=%08X r0-r7=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X sp=%08X\n",
                current_guest_pc, target,
                active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[0] : 0u,
                active_guest_regs ? active_guest_regs[1] : 0u,
                active_guest_regs ? active_guest_regs[2] : 0u,
                active_guest_regs ? active_guest_regs[3] : 0u,
                active_guest_regs ? active_guest_regs[4] : 0u,
                active_guest_regs ? active_guest_regs[5] : 0u,
                active_guest_regs ? active_guest_regs[6] : 0u,
                active_guest_regs ? active_guest_regs[7] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u);
    if (target == 0) {
        fprintf(stderr,
                "guest null indirect branch: pc=0x%08X previous=0x%08X r0=%08X r4=%08X r5=%08X r6=%08X lr=%08X sp=%08X\n",
                current_guest_pc, previous_guest_pc,
                active_guest_regs ? active_guest_regs[0] : 0u,
                active_guest_regs ? active_guest_regs[4] : 0u,
                active_guest_regs ? active_guest_regs[5] : 0u,
                active_guest_regs ? active_guest_regs[6] : 0u,
                active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u);
        /* Optional object callbacks are often still null at this stage of the
           runtime. Returning to LR lets the caller continue; set
           DQ8_STRICT_NULL_CALLBACK=1 to treat a null target as fatal instead. */
        if (!dq8_env_on("DQ8_STRICT_NULL_CALLBACK", &dq8_env_strict_null))
            return active_guest_regs ? active_guest_regs[14] : 0u;
    } else if (target < 0x00100000u || (target >= 0x00600000u && target < 0x30000000u)) {
        /* A non-null target outside the code region is a corrupt function
           pointer (bad vtable, uninitialized thread descriptor, …).
           Do NOT silently skip it — that can mark a worker as finished
           without ever running its entry point. */
        fprintf(stderr,
                "guest invalid indirect branch: pc=0x%08X previous=0x%08X target=%08X r0=%08X r4=%08X r5=%08X r6=%08X lr=%08X sp=%08X\n",
                current_guest_pc, previous_guest_pc, target,
                active_guest_regs ? active_guest_regs[0] : 0u,
                active_guest_regs ? active_guest_regs[4] : 0u,
                active_guest_regs ? active_guest_regs[5] : 0u,
                active_guest_regs ? active_guest_regs[6] : 0u,
                active_guest_regs ? active_guest_regs[14] : 0u,
                active_guest_regs ? active_guest_regs[13] : 0u);
        if (active_guest_regs) {
            uint32_t obj = active_guest_regs[4];
            if (obj < GUEST_RAM_SIZE) {
                fprintf(stderr, "  descriptor@%08X:", obj);
                for (unsigned i = 0; i < 16u && obj + i * 4u + 4u <= GUEST_RAM_SIZE; ++i)
                    fprintf(stderr, " %08X", mem32(obj + i * 4u));
                fputc('\n', stderr);
            }
            fprintf(stderr, "  r0-r3=%08X/%08X/%08X/%08X r8-r12=%08X/%08X/%08X/%08X/%08X\n",
                    active_guest_regs[0], active_guest_regs[1], active_guest_regs[2], active_guest_regs[3],
                    active_guest_regs[8], active_guest_regs[9], active_guest_regs[10],
                    active_guest_regs[11], active_guest_regs[12]);
        }
        exit(30);
    }
    return target;
}
static void svc_break(uint32_t *r) {
    if (r[0] == 0u) return;
    fprintf(stderr, "guest SVC Break: pc=%08X reason=%08X lr=%08X\n",
            current_guest_pc, r[0], r[14]);
    exit(12);
}
static void guest_udf(uint32_t immediate) {
    fprintf(stderr, "guest ARM UDF reached: pc=%08X immediate=0x%06X lr=%08X sp=%08X\n",
            current_guest_pc, immediate,
            active_guest_regs ? active_guest_regs[14] : 0u,
            active_guest_regs ? active_guest_regs[13] : 0u);
    exit(14);
}
static void guest_unsupported(uint32_t pc, uint32_t opcode, uint32_t thumb) {
    fprintf(stderr, "unsupported guest instruction: mode=%s pc=0x%08X opcode=0x%08X\n",
            thumb ? "Thumb" : "ARM", pc, opcode);
    exit(15);
}
static uint32_t svc_create_address_arbiter(uint32_t *r) {
    if (next_arbiter_handle >= 0x20040u) { fputs("native guest arbiter table exhausted\n", stderr); exit(16); }
    uint32_t handle = next_arbiter_handle++;
    arbiter_live[handle - 0x20000u] = 1;
    /* This game's syscall veneer consumes the created handle from r1 and
       stores it through its saved output pointer after the SVC returns. */
    r[1] = handle;
    return 0;
}
static uint32_t svc_arbitrate(uint32_t handle, uint32_t address, uint32_t type,
                              uint32_t value, uint32_t timeout_lo, uint32_t timeout_hi) {
    /* The guest's startup veneer currently loses the returned handle while
       unwinding its temporary stack slot.  Preserve the single live object. */
    if (handle == 0 && next_arbiter_handle == 0x20001u) handle = 0x20000u;
    if (handle < 0x20000u || handle >= 0x20040u || !arbiter_live[handle - 0x20000u]) {
        /* STOP-1 diagnostic: identify which guest call site passed a handle the
           arbiter table does not own (the pre-fix message only carried the
           handle value, so the caller could not be traced back to its veneer). */
        /* STOP-1 follow-up (t_be5fc82c): pin the writer of the handle word.
           Two discriminating reads plus the caller's PRE-veneer r4/r5: the
           veneer at 0x0012C340 does `stmdb sp!,{r4,r5}` before the SVC, so at
           the svc pc the caller's original r4/r5 sit at [sp] / [sp+4] while
           active_guest_regs[4]/[5] hold the clobbered timeout slots. */
        uint32_t _sp = active_guest_regs ? active_guest_regs[13] : 0u;
        uint32_t _call_r4 = (_sp + 4u < GUEST_RAM_SIZE) ? mem32(_sp) : 0xDEAD0001u;
        uint32_t _call_r5 = (_sp + 8u < GUEST_RAM_SIZE) ? mem32(_sp + 4u) : 0xDEAD0002u;
        /* Stack layout at the svc (veneer stmdb {r4,r5} on top of the wrapper's
           stmdb {r4,r5,r6,lr} + sp-=8 + stmia {r4,ip}): [+8]/[+0xC] are the
           timeout slots (expect 0/0), [+0x1C] is the wrapper's return address
           into ITS caller -> pins how 0x0014C914 was entered.  lit14C9C8 is the
           in-RAM literal the wrapper's `ldr r5,[pc,#0x9C]` loads (file value
           0x005E94F0) - a zero here means the code region was clobbered. */
        uint32_t _slot8 = (_sp + 0x10u < GUEST_RAM_SIZE) ? mem32(_sp + 8u) : 0xDEAD0003u;
        uint32_t _slotC = (_sp + 0x14u < GUEST_RAM_SIZE) ? mem32(_sp + 0xCu) : 0xDEAD0004u;
        uint32_t _up_lr = (_sp + 0x20u < GUEST_RAM_SIZE) ? mem32(_sp + 0x1Cu) : 0xDEAD0005u;
        fprintf(stderr,
                "SVC ArbitrateAddress: invalid handle=%08X pc=%08X prev=%08X lr=%08X sp=%08X "
                "depth=%u thread=%08X addr=%08X type=%u value=%08X timeout=%08X:%08X "
                "r4=%08X r5=%08X r6=%08X r7=%08X "
                "arbiter_created_next=%08X live0=%u "
                "g005E94F0=%08X g00000000=%08X call_r4=%08X call_r5=%08X "
                "lit14C9C8=%08X slot8=%08X slotC=%08X up_lr=%08X\n",
                handle, current_guest_pc, previous_guest_pc,
                active_guest_regs ? active_guest_regs[14] : 0u,
                _sp,
                thread_execution_depth, current_guest_thread_id, address, type, value,
                timeout_hi, timeout_lo,
                active_guest_regs ? active_guest_regs[4] : 0u,
                active_guest_regs ? active_guest_regs[5] : 0u,
                active_guest_regs ? active_guest_regs[6] : 0u,
                active_guest_regs ? active_guest_regs[7] : 0u,
                next_arbiter_handle,
                arbiter_live[0],
                mem32(0x005E94F0u),
                mem32(0u),
                _call_r4,
                _call_r5,
                mem32(0x0014C9C8u),
                _slot8,
                _slotC,
                _up_lr);
        exit(13);
    }
    if (address >= GUEST_RAM_SIZE || (address & 3u)) return 0xD8E007F7u;
    static unsigned arbiter_trace_count;
    static unsigned arbiter_wait_trace_count;
    if (!dq8_env_arbiter_addr_init) {
        /* t_275eb521: the trace address was re-parsed with strtoul on every
           arbiter operation; parse it once. */
        const char *text = getenv("DQ8_TRACE_ARBITER_ADDRESS");
        dq8_env_arbiter_addr_init = 1;
        dq8_env_arbiter_addr = text ? (uint32_t)strtoul(text, NULL, 0) : 0xFFFFFFFFu;
    }
    uint32_t arbiter_trace_address = dq8_env_arbiter_addr;
    int trace_this_arbiter = address == arbiter_trace_address;
    if (trace_this_arbiter || (dq8_env_on("DQ8_TRACE_ARBITER", &dq8_env_trace_arbiter) && arbiter_trace_count++ < 256u))
        fprintf(stderr, "guest arbiter pc=%08X depth=%u handle=%08X addr=%08X type=%u value=%08X word=%08X\n",
                current_guest_pc, thread_execution_depth, handle, address, type, value, mem32(address));
    if ((trace_this_arbiter || dq8_env_on("DQ8_TRACE_ARBITER", &dq8_env_trace_arbiter)) && type != 0u &&
        (trace_this_arbiter || arbiter_wait_trace_count++ < 128u))
        fprintf(stderr, "guest arbiter wait pc=%08X depth=%u handle=%08X addr=%08X type=%u value=%08X timeout=%08X:%08X word=%08X\n",
                current_guest_pc, thread_execution_depth, handle, address, type, value,
                timeout_hi, timeout_lo, mem32(address));
    switch (type) {
    case 0: {
        /* Signal is a wake count, not an arithmetic update to guest memory.
           Preserve priority ordering among represented worker waiters.  The
           main guest is an implicit waiter because it runs on the host stack;
           consume a signal for it only after eligible worker waiters. */
        int main_waiter = main_arbiter_waiting && main_arbiter_handle == handle &&
                          main_arbiter_address == address && !main_arbiter_signaled;
        unsigned remaining = (int32_t)value < 0
            ? guest_thread_count + (main_waiter ? 1u : 0u) : value;
        while (remaining) {
            GuestThread *best = NULL;
            for (unsigned i = 0; i < guest_thread_count; ++i) {
                GuestThread *thread = &guest_threads[i];
                if (!thread->active || !thread->blocked || thread->wait_arbiter != handle ||
                    thread->wait_address != address) continue;
                if (!best || thread->priority < best->priority) best = thread;
            }
            if (best) {
                /* Timed arbiter calls report the kernel timeout result even
                   when a signal is what releases the waiter (Azahar contract). */
                uint32_t result = best->wait_type >= 3u ? 0x09401BFEu : 0u;
                guest_wait_finish(best, result, 0u, 0);
                --remaining;
                continue;
            }
            if (main_waiter && !main_arbiter_signaled) {
                main_arbiter_signaled = 1u;
                --remaining;
                continue;
            }
            break;
        }
        return 0;
    }
    case 1: case 2: case 3: case 4: {
        int timed = type >= 3u;
        int decrement = type == 2u || type == 4u;
        uint64_t timeout = (uint64_t)timeout_lo | ((uint64_t)timeout_hi << 32);
        uint32_t current = mem32(address);
        if ((int32_t)current >= (int32_t)value)
            return timed ? 0x09401BFEu : 0u;
        if (decrement) put32(address, current - 1u);

        /* Azahar's timed variants call WakeAfterDelay(0) and return timeout;
           they do not park when the requested duration is already zero. */
        if (timed && timeout == 0u) return 0x09401BFEu;
        if (thread_execution_depth) {
            thread_yield_requested = 1;
            thread_blocked_requested = 1;
            thread_wait_address_requested = address;
            thread_wait_arbiter_requested = handle;
            thread_wait_type_requested = type;
            thread_wait_value_requested = value;
            thread_sleep_requested_ns = timed && (int64_t)timeout > 0 ? idle_stretch_wait(timeout) : 0u;
            thread_wake_result_requested = timed ? 0x09401BFEu : 0u;
            return timed ? 0x09401BFEu : 0u;
        }

        /* The main context cannot be inserted into guest_threads[].  Keep its
           arbiter identity visible to type-0 signals, run bounded peer slices,
           and sleep only when no worker is runnable.  The waited-on address
           and the signal path above are the proven read set and wake source. */
        main_arbiter_waiting = 1u;
        main_arbiter_signaled = 0u;
        main_arbiter_handle = handle;
        main_arbiter_address = address;
        uint64_t deadline = timed && (int64_t)timeout > 0
            ? dq8_guest_now_ns() + idle_stretch_wait(timeout) : 0u;
        for (;;) {
            schedule_guest_thread();
            uint64_t now = dq8_guest_now_ns();
            guest_service_host_deadlines(now);
            if (main_arbiter_signaled) break;
            if (deadline && now >= deadline) break;
            if (guest_any_runnable_thread()) continue;
            if (!runnable_nap()) { guest_pacing_nap(deadline, 1000000ull); continue; }
            guest_wait_until_deadline(deadline);
        }
        int signaled = main_arbiter_signaled;
        main_arbiter_waiting = 0u;
        main_arbiter_signaled = 0u;
        main_arbiter_handle = main_arbiter_address = 0u;
        if (timed) return 0x09401BFEu;
        return signaled ? 0u : 0x09401BFEu;
    }
    default:
        fprintf(stderr, "SVC ArbitrateAddress: unsupported type=%u value=%08X\n", type, value);
        exit(13);
    }
}
/* NATIVE_FS_RUNTIME */
/* NATIVE_CECD_RUNTIME */
/* ---------------------------------------------------------------------------
 * y2r:u — the 3DS YUV→RGB converter used by the opening movie.
 *
 * This is a faithful port of Azahar's HW::Y2R::PerformConversion
 * (azahar-master/src/core/hw/y2r.cpp) plus the y2r:u command table
 * (azahar-master/src/core/hle/service/cam/y2r_u.cpp): the engine works on
 * 8-row strips, converts each pixel with the configured fixed-point
 * coefficients, optionally rotates each strip, writes the result either
 * linearly or as 8x8 Morton-tiled blocks, and finally DMAs it out honouring
 * the per-plane transfer_unit/gap strides that the guest configured with
 * SetSending / SetReceiving.  The transfer-end event is signalled when the
 * conversion finishes, which is what releases the movie presentation loop.
 * ------------------------------------------------------------------------- */
struct Y2RBuf { uint32_t addr, size, unit, gap; };
struct Y2RCfg {
    uint32_t in_fmt, out_fmt, rotation, align;
    uint32_t width, lines;
    int32_t coef[8];
    uint32_t alpha;
    uint32_t irq_enable;
    struct Y2RBuf src_y, src_u, src_v, src_yuyv, dst;
    uint32_t end_event;
    uint32_t conversions;
};
static struct Y2RCfg y2r;
static int y2r_inited;
/* Azahar's standard_coefficients[] (ITU Rec.601 / Rec.709, PC and TV range). */
static const int32_t y2r_std_coef[4][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B},
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B},
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},
};
static const uint8_t y2r_lut_linear[64] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63
};
static const uint8_t y2r_lut_morton[64] = {
     0,  1,  4,  5, 16, 17, 20, 21,  2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63
};
/* Scratch buffers sized for the maximum legal configuration (width 1024). */
static uint8_t y2r_strip[1024u * 8u * 4u];
static uint32_t y2r_tiles[128][64];
static uint32_t y2r_out_tile[64];

static void y2r_driver_init(void) {
    uint32_t keep_event = y2r.end_event;
    memset(&y2r, 0, sizeof y2r);
    y2r.end_event = keep_event;      /* the handle object survives a re-init */
    y2r.in_fmt = 0u;                 /* YUV422_Indiv8 */
    y2r.out_fmt = 0u;                /* RGBA8 */
    y2r.rotation = 0u; y2r.align = 0u;
    y2r.width = 1024u; y2r.lines = 1024u;
    y2r.alpha = 0u;
    y2r_inited = 1;
}

/* DMA one plane region into `out`, honouring transfer_unit/gap strides.
   `n` is the bytes per sample (1 for 8-bit input, 2 for 16-bit input). */
static void y2r_receive(struct Y2RBuf *b, uint8_t *out, uint32_t amount, uint32_t n) {
    uint32_t output_unit = n ? b->unit / n : 0u;
    if (!output_unit) { memset(out, 0x80u, amount); return; }
    while (amount >= output_unit) {
        uint32_t mapped = 0;
        if (b->unit && guest_memory_range(b->addr, b->unit, &mapped)) {
            const uint8_t *in = ram + mapped;
            for (uint32_t i = 0; i < output_unit; ++i) out[i] = in[i * n];
        } else {
            memset(out, 0x80u, output_unit);
        }
        out += output_unit;
        b->addr += b->unit + b->gap;
        if (b->size >= b->unit) b->size -= b->unit;
        amount -= output_unit;
    }
    if (amount) memset(out, 0x80u, amount);
}

/* Decode one strip of pixels into RGB32 tiles (Azahar ConvertYUVToRGB). */
static void y2r_decode_strip(const uint8_t *Y, const uint8_t *U, const uint8_t *V,
                             uint32_t width, uint32_t height) {
    const int32_t *c = y2r.coef;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            int32_t yv, uv, vv;
            switch (y2r.in_fmt) {
            case 0u: case 2u: /* YUV422_Indiv8 / Indiv16 */
                yv = Y[y * width + x];
                uv = U[(y * width + x) / 2u];
                vv = V[(y * width + x) / 2u];
                break;
            case 1u: case 3u: /* YUV420_Indiv8 / Indiv16 */
                yv = Y[y * width + x];
                uv = U[((y / 2u) * width + x) / 2u];
                vv = V[((y / 2u) * width + x) / 2u];
                break;
            default: /* YUYV422_Interleaved: U/V alias the Y buffer */
                yv = Y[(y * width + x) * 2u];
                uv = U[(y * width + (x / 2u) * 2u) * 2u + 1u];
                vv = V[(y * width + (x / 2u) * 2u) * 2u + 3u];
                break;
            }
            int32_t cy = c[0] * yv;
            int32_t r = cy + c[1] * vv;
            int32_t g = cy - c[2] * vv - c[3] * uv;
            int32_t b = cy + c[4] * uv;
            const int32_t rounding_offset = 0x18;
            r = (r >> 3) + c[5] + rounding_offset;
            g = (g >> 3) + c[6] + rounding_offset;
            b = (b >> 3) + c[7] + rounding_offset;
            r = r >> 5; if (r < 0) r = 0; if (r > 255) r = 255;
            g = g >> 5; if (g < 0) g = 0; if (g > 255) g = 255;
            b = b >> 5; if (b < 0) b = 0; if (b > 255) b = 255;
            uint32_t tile = x / 8u, tile_x = x % 8u;
            y2r_tiles[tile][y * 8u + tile_x] =
                ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8);
        }
    }
}

/* Rotate one strip tile and write it into the intermediate output buffer
   (Azahar RotateTile0/90/180/270 and WriteTileToOutput merged). Returns pixels written. */
static uint32_t y2r_emit_strip(uint32_t *out, uint32_t row_height, uint32_t num_tiles) {
    const uint8_t *remap = (y2r.align == 0u) ? y2r_lut_linear : y2r_lut_morton;
    uint32_t written = 0;
    for (uint32_t i = 0; i < num_tiles; ++i) {
        uint32_t strip_width = 0, output_stride = 0;
        const uint32_t *src = y2r_tiles[i];
        switch (y2r.rotation) {
        case 0u: /* None */
            for (uint32_t k = 0; k < row_height * 8u; ++k) y2r_out_tile[remap[k]] = src[k];
            strip_width = y2r.width; output_stride = 8u;
            break;
        case 1u: { /* Clockwise_90 */
            unsigned n = 0;
            for (uint32_t tx = 0; tx < 8u; ++tx)
                for (uint32_t ty = row_height; ty-- > 0u; )
                    y2r_out_tile[remap[n++]] = src[ty * 8u + tx];
            strip_width = 8u; output_stride = 8u * row_height;
            break; }
        case 2u: /* Clockwise_180 */
            src = y2r_tiles[num_tiles - i - 1u];
            for (uint32_t k = 0; k < row_height * 8u; ++k)
                y2r_out_tile[remap[k]] = src[row_height * 8u - 1u - k];
            strip_width = y2r.width; output_stride = 8u;
            break;
        default: { /* Clockwise_270 */
            src = y2r_tiles[num_tiles - i - 1u];
            unsigned n = 0;
            for (uint32_t tx = 8u; tx-- > 0u; )
                for (uint32_t ty = 0; ty < row_height; ++ty)
                    y2r_out_tile[remap[n++]] = src[ty * 8u + tx];
            strip_width = 8u; output_stride = 8u * row_height;
            break; }
        }
        if (y2r.align == 0u) { /* Linear */
            for (uint32_t ty = 0; ty < row_height; ++ty)
                for (uint32_t tx = 0; tx < 8u; ++tx)
                    out[ty * strip_width + tx] = y2r_out_tile[ty * 8u + tx];
            out += output_stride;
            written += row_height * 8u;
        } else { /* Block8x8: whole 8x8 block, always in Morton order */
            for (uint32_t k = 0; k < 64u; ++k) out[k] = y2r_out_tile[k];
            out += 64u;
            written += 64u;
        }
    }
    return written;
}

/* DMA the converted strip out to the guest destination (Azahar SendData). */
static void y2r_send(const uint32_t *input, uint32_t pixels, struct Y2RBuf *b, uint32_t bpp) {
    while (pixels) {
        uint32_t mapped = 0;
        uint8_t *out = NULL;
        if (b->unit && guest_memory_range(b->addr, b->unit, &mapped)) out = ram + mapped;
        uint32_t unit_pixels = b->unit / bpp;
        uint32_t n = unit_pixels < pixels ? unit_pixels : pixels;
        if (!out) { /* destination not addressable: skip the transfer unit */
            pixels -= n; b->addr += b->unit + b->gap;
            if (b->size >= b->unit) b->size -= b->unit;
            continue;
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t color = input[i];
            uint8_t r = (uint8_t)(color >> 24), g = (uint8_t)(color >> 16),
                    bl = (uint8_t)(color >> 8), a = (uint8_t)y2r.alpha;
            uint8_t *o = out + i * bpp;
            switch (y2r.out_fmt) {
            case 0u: o[0] = a; o[1] = bl; o[2] = g; o[3] = r; break;      /* RGBA8 */
            case 1u: o[0] = bl; o[1] = g; o[2] = r; break;                /* RGB8 */
            case 2u: { uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) |
                             ((bl >> 3) << 1) | (a >> 7));                 /* RGB5A1 */
                      o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); break; }
            default: { uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) |
                              (bl >> 3));                                  /* RGB565 */
                       o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); break; }
            }
        }
        pixels -= n;
        b->addr += b->unit + b->gap;
        if (b->size >= b->unit) b->size -= b->unit;
    }
}

static void y2r_signal_end(void) {
    if (!y2r.end_event || y2r.end_event >= next_event_handle || !event_live[y2r.end_event]) return;
    event_signaled[y2r.end_event] = 1u;
    guest_notify_handle(y2r.end_event);
}

/* Statistics over one input plane region: proves whether the guest really
   decoded video into the YUV buffers it submitted to the engine. */
static void y2r_plane_stats(const char *name, uint32_t addr, uint32_t size,
                            uint32_t unit, uint32_t gap, uint32_t rows) {
    uint32_t mn = 255u, mx = 0u, count = 0, buckets = 0;
    uint64_t sum = 0;
    uint32_t seen[8] = {0};
    uint32_t stride = unit + gap;
    for (uint32_t r = 0; rows && r < rows; ++r) {
        uint32_t base = addr + r * stride;
        for (uint32_t i = 0; i < unit; ++i) {
            uint32_t m;
            if (!guest_memory_range(base + i, 1u, &m)) continue;
            uint8_t v = ram[m];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v; ++count;
            seen[v >> 5] = 1u;
        }
    }
    for (unsigned i = 0; i < 8u; ++i) buckets += seen[i];
    fprintf(stderr, "Y2R plane %s addr=%08X size=%u unit=%u gap=%u rows=%u "
                    "min=%u max=%u mean=%u value_buckets=%u\n",
            name, addr, size, unit, gap, rows,
            count ? mn : 0u, count ? mx : 0u,
            count ? (unsigned)(sum / count) : 0u, buckets);
}

/* Dump one converted frame (RGB8/RGBA8/RGB565, linear or Morton-tiled) as a
   PPM so the YUV→RGB result can be verified independently of the GPU
   presentation path. */
static void y2r_dump_frame(const char *prefix, uint32_t index, uint32_t width,
                           uint32_t lines, uint32_t bpp, struct Y2RBuf dst) {
    char path[512];
    snprintf(path, sizeof path, "%s-%04u.ppm", prefix, index);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Y2R dump %s: open failed\n", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", width, lines);
    uint32_t strips = (lines + 7u) / 8u;
    for (uint32_t s = 0; s < strips; ++s) {
        uint32_t row_height = (lines - s * 8u < 8u) ? (lines - s * 8u) : 8u;
        uint32_t addr = dst.addr + s * (dst.unit + dst.gap);
        for (uint32_t ty = 0; ty < row_height; ++ty) {
            for (uint32_t tx = 0; tx < width; ++tx) {
                uint32_t byte_off;
                if (y2r.align == 0u) {
                    byte_off = (ty * width + tx) * bpp;
                } else {
                    uint32_t tile = (tx / 8u) + (ty / 8u) * (width / 8u);
                    uint32_t idx = (ty % 8u) * 8u + (tx % 8u);
                    byte_off = (tile * 64u + y2r_lut_morton[idx]) * bpp;
                }
                uint32_t m, r = 0, g = 0, b = 0;
                if (guest_memory_range(addr + byte_off, bpp, &m)) {
                    if (bpp == 3u) { b = ram[m]; g = ram[m + 1]; r = ram[m + 2]; }
                    else if (bpp == 4u) { b = ram[m + 1]; g = ram[m + 2]; r = ram[m + 3]; }
                    else { uint32_t v = (uint32_t)ram[m] | ((uint32_t)ram[m + 1] << 8);
                           r = (v >> 11) << 3; g = ((v >> 5) & 63u) << 2; b = (v & 31u) << 3; }
                }
                fputc((int)r, f); fputc((int)g, f); fputc((int)b, f);
            }
        }
    }
    fclose(f);
    if (getenv("DQ8_TRACE_Y2R")) fprintf(stderr, "Y2R wrote %s\n", path);
}

static void y2r_start_conversion(void) {
    uint64_t y2r_t0 = movie_trace() ? host_monotonic_ns() : 0u;
    if (!y2r_inited) y2r_driver_init();
    uint32_t width = y2r.width, lines = y2r.lines;
    if (!width || width > 1024u || (width % 8u) || !lines || lines > 1024u) {
        fprintf(stderr, "Y2R StartConversion rejected width=%u lines=%u\n", width, lines);
        y2r_signal_end();
        return;
    }
    uint32_t num_tiles = width / 8u;
    ++perf_y2r_conversions;
    /* PERF-01 game-level regression evidence: one timestamped line per
       completed conversion.  The inter-arrival intervals give the frame
       arrival cadence (p50/p95/max) and n=1 marks the first converted frame.
       Env-gated; no behaviour change when DQ8_TRACE_Y2R is unset. */
    if (getenv("DQ8_TRACE_Y2R"))
        fprintf(stderr, "Y2R_ARRIVAL n=%llu t_ns=%llu\n",
                (unsigned long long)perf_y2r_conversions,
                (unsigned long long)host_monotonic_ns());
    /* Each conversion restarts every DMA from the configured address (Azahar
       passes the configuration to PerformConversion by value), so operate on
       local copies of the buffers. */
    struct Y2RBuf sy = y2r.src_y, su = y2r.src_u, sv = y2r.src_v;
    struct Y2RBuf syuyv = y2r.src_yuyv, sd = y2r.dst;
    if (getenv("DQ8_TRACE_Y2R")) {
        if (y2r.in_fmt == 4u) {
            y2r_plane_stats("YUYV", syuyv.addr, syuyv.size, syuyv.unit, syuyv.gap, lines);
        } else {
            y2r_plane_stats("Y", sy.addr, sy.size, sy.unit, sy.gap, lines);
            y2r_plane_stats("U", su.addr, su.size, su.unit, su.gap, lines / 2u);
            y2r_plane_stats("V", sv.addr, sv.size, sv.unit, sv.gap, lines / 2u);
        }
    }
    for (uint32_t y = 0; y < lines; y += 8u) {
        uint32_t row_height = (lines - y < 8u) ? (lines - y) : 8u;
        uint32_t row_data_size = row_height * width;   /* pixels in this strip */
        uint32_t c_amount = (y2r.in_fmt == 0u || y2r.in_fmt == 2u)
                                ? row_data_size / 2u : row_data_size / 4u;
        uint32_t y_amount = (y2r.in_fmt == 4u) ? row_data_size * 2u : row_data_size;
        uint8_t *in_y = y2r_strip;
        uint8_t *in_u = in_y + 8u * width;
        uint8_t *in_v = in_u + 8u * width / 2u;
        const uint8_t *src_u, *src_v;
        if (y2r.in_fmt == 4u) {
            y2r_receive(&syuyv, in_y, y_amount, 1u);
            src_u = src_v = in_y;
        } else {
            uint32_t n = (y2r.in_fmt & 2u) ? 2u : 1u;
            y2r_receive(&sy, in_y, y_amount, n);
            y2r_receive(&su, in_u, c_amount, n);
            y2r_receive(&sv, in_v, c_amount, n);
            src_u = in_u; src_v = in_v;
        }
        y2r_decode_strip(in_y, src_u, src_v, width, row_height);
        uint32_t *strip_out = (uint32_t *)(void *)y2r_strip;
        y2r_emit_strip(strip_out, row_height, num_tiles);
        y2r_send(strip_out, row_data_size, &sd,
                 (y2r.out_fmt == 1u) ? 3u : ((y2r.out_fmt == 0u) ? 4u : 2u));
    }
    y2r.conversions++;
    if (getenv("DQ8_TRACE_Y2R"))
        fprintf(stderr, "Y2R conversion #%u w=%u h=%u in=%u out=%u rot=%u align=%u "
                        "coef0=%d dst=%08X dstsize=%u unit=%u gap=%u\n",
                y2r.conversions, width, lines, y2r.in_fmt, y2r.out_fmt, y2r.rotation,
                y2r.align, y2r.coef[0], y2r.dst.addr, y2r.dst.size, y2r.dst.unit, y2r.dst.gap);
    {
        const char *prefix = getenv("DQ8_DUMP_Y2R_PREFIX");
        if (prefix)
            y2r_dump_frame(prefix, y2r.conversions, width, lines,
                           (y2r.out_fmt == 1u) ? 3u : ((y2r.out_fmt == 0u) ? 4u : 2u), y2r.dst);
    }
    if (y2r_t0) {
        uint64_t y2r_dt = host_monotonic_ns() - y2r_t0;
        movie_trace_y2r_ns += y2r_dt;
        if (y2r_dt > movie_trace_y2r_max_ns) movie_trace_y2r_max_ns = y2r_dt;
        ++movie_trace_y2r_count;
        movie_trace_event("y2r_done n=%llu w=%u h=%u in=%u out=%u us=%llu dst=%08X",
                          (unsigned long long)y2r.conversions, width, lines,
                          y2r.in_fmt, y2r.out_fmt,
                          (unsigned long long)(y2r_dt / 1000ull), y2r.dst.addr);
    }
    y2r_signal_end();
}

static uint32_t svc_send_sync(uint32_t handle) {
    uint32_t command = mem32(active_tls_base + 0x80u);
    if (getenv("DQ8_DSP_DUMP")) dsp_dump_maybe();
    static unsigned ipc_trace_count;
    if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc) && ipc_trace_count++ < 20000u)
        fprintf(stderr, "guest IPC pc=%08X lr=%08X handle=%08X command=%08X args=%08X/%08X/%08X/%08X\n",
                current_guest_pc, active_guest_regs ? active_guest_regs[14] : 0u,
                handle, command, mem32(active_tls_base + 0x84u), mem32(active_tls_base + 0x88u),
                mem32(active_tls_base + 0x8Cu), mem32(active_tls_base + 0x90u));
    if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc) && handle >= 0x20u && handle < 0x100u && command == 0x080200C2u)
        fprintf(stderr, "FS Read handle=%08X offset=%08X:%08X size=%08X buffer=%08X\n", handle,
                mem32(active_tls_base + 0x88u), mem32(active_tls_base + 0x84u), mem32(active_tls_base + 0x8Cu),
                mem32(active_tls_base + 0x94u));
    if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc) && handle == 8u && command == 0x08030204u) {
        uint32_t ap = mem32(active_tls_base + 0xA8u), fp = mem32(active_tls_base + 0xB0u);
        fprintf(stderr, "FS OpenFileDirectly pc=%08X archive=%08X archivePathType=%08X archivePathSize=%08X filePathType=%08X filePathSize=%08X flags=%08X raw=",
                current_guest_pc,
                mem32(active_tls_base + 0x88u), mem32(active_tls_base + 0x8Cu), mem32(active_tls_base + 0x90u),
                mem32(active_tls_base + 0x94u), mem32(active_tls_base + 0x98u), mem32(active_tls_base + 0x9Cu));
        for (unsigned i = 0; i < 20u; ++i) fprintf(stderr, "%08X%s", mem32(active_tls_base + 0x80u + i * 4u), i == 19u ? " " : "/");
        fprintf(stderr, "archivePath@%08X=", ap);
        for (unsigned i = 0; i < 64u && ap + i < GUEST_RAM_SIZE; ++i) fprintf(stderr, "%02X", mem8(ap + i));
        fprintf(stderr, " filePath=");
        for (unsigned i = 0; i < 128u && fp + i < GUEST_RAM_SIZE; ++i) fprintf(stderr, "%02X", mem8(fp + i));
        fputc('\n', stderr);
    }
    if (getenv("DQ8_TRACE_FS_SERVICE") && handle == 8u &&
        (command == 0x080C00C2u || command == 0x080201C2u || command == 0x08040000u ||
         command == 0x08080000u || command == 0x080E0080u)) {
        fprintf(stderr, "FS service request pc=%08X cmd=%08X words=", current_guest_pc, command);
        for (unsigned i = 0; i < 24u; ++i)
            fprintf(stderr, "%08X%s", mem32(active_tls_base + 0x80u + i * 4u), i == 23u ? "\\n" : "/");
        for (unsigned off = 0x84u; off <= 0xB8u; off += 4u) {
            uint32_t candidate = mem32(active_tls_base + off);
            if (candidate < GUEST_RAM_SIZE && candidate >= 0x10000u) {
                fprintf(stderr, "  candidate@+%02X=%08X bytes=", off, candidate);
                for (unsigned i = 0; i < 48u && candidate + i < GUEST_RAM_SIZE; ++i)
                    fprintf(stderr, "%02X", mem8(candidate + i));
                fputc('\n', stderr);
            }
        }
    }
    if (native_fs_dispatch(handle, command)) return 0;
    if (native_cecd_dispatch(handle, command)) return 0;
    if (handle >= 0x20u && handle < 0x100u && command == 0x080200C2u) {
        if (guest_memfile_data[handle]) {
            /* NCCH archive file backed by in-memory image data.  Read follows
               azahar IVFCFileInMemory semantics (azahar file backend Read):
               read_length = min(length, data_size - offset); an offset past
               data_size underflows u64 there and yields a full-length read,
               which we emulate as a full-length zero fill instead of reading
               out of bounds. */
            uint32_t size = mem32(active_tls_base + 0x8Cu);
            uint32_t output = mem32(active_tls_base + 0x94u);
            uint32_t mapped_output;
            uint64_t offset = (uint64_t)mem32(active_tls_base + 0x84u) |
                              ((uint64_t)mem32(active_tls_base + 0x88u) << 32);
            uint32_t data_size = guest_memfile_size[handle];
            uint32_t bytes_read = 0;
            if (size > 0x04000000u || !guest_memory_range(output, size, &mapped_output)) {
                put32(active_tls_base + 0x84u, 0u);
                put32(active_tls_base + 0x88u, 0u);
                return 0;
            }
            if (offset <= (uint64_t)data_size) {
                uint64_t available = (uint64_t)data_size - offset;
                bytes_read = (uint64_t)size < available ? size : (uint32_t)available;
                if (bytes_read)
                    memcpy(ram + mapped_output, guest_memfile_data[handle] + (size_t)offset,
                           bytes_read);
            } else {
                bytes_read = size; /* azahar underflow path: full length */
            }
            if (bytes_read < size)
                memset(ram + mapped_output + bytes_read, 0, size - bytes_read);
            put32(active_tls_base + 0x84u, 0u);
            put32(active_tls_base + 0x88u, bytes_read);
            return 0;
        }
        if (!guest_file_is_romfs[handle]) {
            /* Save/non-RomFS file: return zeroed data so the guest can
               parse it as a fresh or empty save rather than failing. */
            uint32_t size = mem32(active_tls_base + 0x8Cu);
            uint32_t output = mem32(active_tls_base + 0x94u);
            uint32_t mapped_output;
            if (size > 0x04000000u || !guest_memory_range(output, size, &mapped_output)) {
                put32(active_tls_base + 0x80u, 0x080200C2u);
                put32(active_tls_base + 0x84u, 0u);
                put32(active_tls_base + 0x88u, 0u);
                return 0;
            }
            memset(ram + mapped_output, 0, size);
            put32(active_tls_base + 0x84u, 0u);
            put32(active_tls_base + 0x88u, size);
            return 0;
        }
        uint32_t size = mem32(active_tls_base + 0x8Cu);
        uint32_t output = mem32(active_tls_base + 0x94u);
        uint32_t mapped_output;
        uint64_t offset = (uint64_t)mem32(active_tls_base + 0x84u) |
                          ((uint64_t)mem32(active_tls_base + 0x88u) << 32);
        if (!romfs_image || size > 0x04000000u ||
            !guest_memory_range(output, size, &mapped_output)) {
            fprintf(stderr, "invalid synthetic FS read size=%08X output=%08X\n", size, output);
            exit(10);
        }
        uint32_t bytes_read = 0;
        if (offset < romfs_data_size) {
            uint64_t available = romfs_data_size - offset;
            if (size > available) size = (uint32_t)available;
            if (fseeko(romfs_image, (off_t)(romfs_data_offset + offset), SEEK_SET) == 0)
                bytes_read = (uint32_t)fread(ram + mapped_output, 1, size, romfs_image);
        }
        if (movie_trace()) {
            ++movie_trace_fs_reads;
            movie_trace_fs_bytes += bytes_read;
            movie_trace_event("fs_read handle=%08X offset=%llu size=%u got=%u",
                              handle, (unsigned long long)offset, size, bytes_read);
        }
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, bytes_read);
        return 0;
    }
    if (handle >= 0x20u && handle < 0x100u &&
        (command == 0x08080000u || command == 0x08090000u || command == 0x08040000u)) {
        put32(active_tls_base + 0x84u, 0u);
        if (command == 0x08080000u && handle < 256u)
            guest_memfile_data[handle] = NULL; /* FSFILE:Close */
        if (command == 0x08040000u) {
            uint64_t size = guest_file_is_romfs[handle] ? romfs_data_size : 0x1000u;
            if (handle < 256u && guest_memfile_data[handle])
                size = guest_memfile_size[handle]; /* azahar: file content size */
            put32(active_tls_base + 0x88u, (uint32_t)size);
            put32(active_tls_base + 0x8Cu, (uint32_t)(size >> 32));
        }
        return 0;
    }
    if (handle >= 0x20u && handle < 0x100u && command == 0x080C0000u) {
        /* FSFILE:OpenLinkFile on a synthetic (RomFS) file handle. Azahar's
           implementation (azahar file.cpp:385) is a stub that returns a fresh
           client session to the same file backend with offset reset. RomFS
           reads in this runtime carry their offset explicitly on every call
           and there is no per-handle cursor to copy, so the alias only needs
           a new handle id flagged with the same RomFS bit. Reached during the
           opening-movie file setup (handle 0x26). */
        uint32_t link = guest_alloc_ipc_handle();
        if (link < sizeof(guest_file_is_romfs)) {
            guest_file_is_romfs[link] = guest_file_is_romfs[handle];
            guest_memfile_data[link] = guest_memfile_data[handle];
            guest_memfile_size[link] = guest_memfile_size[handle];
        }
        put32(active_tls_base + 0x80u, 0x080C0042u); /* MakeBuilder -> 1 move handle */
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x10u); /* MoveHandleDesc(1) */
        put32(active_tls_base + 0x8Cu, link);
        return 0;
    }
    if (handle >= 0x20u && handle < 0x100u &&
        ((command >> 16) == 0x080Au || (command >> 16) == 0x080Bu)) {
        /* FSFILE:SetPriority (0x080A) / GetPriority (0x080B). Priority is only
           an I/O-scheduling hint with no effect on this synchronous runtime;
           store it per handle so Set/Get round-trip consistently. Response for
           Set is the Result word alone (counts 0x40); Get adds the priority
           (counts 0x80), matching azahar file.cpp MakeBuilder(1,0)/(2,0). */
        static uint32_t guest_file_priority[256];
        if ((command >> 16) == 0x080Au)
            guest_file_priority[handle] = mem32(active_tls_base + 0x84u);
        put32(active_tls_base + 0x80u,
              (command & 0xFFFF0000u) | ((command >> 16) == 0x080Bu ? 0x80u : 0x40u));
        put32(active_tls_base + 0x84u, 0u);
        if ((command >> 16) == 0x080Bu)
            put32(active_tls_base + 0x88u, guest_file_priority[handle]);
        return 0;
    }
    /* "cecd:u" (handle 17) is the CECD/StreetPass service: the game asks for it
       right after the adventure log is created (the name-entry screen warned
       about StreetPass).  Its whole command table lives in native_cecd.c. */
    static const char *service_names[] = { "APT:A", "APT:U", "gsp::Gpu", "fs:USER", "ndm:u", "ptm:u", "hid:USER", "ir:USER", "cfg:u", "dsp::DSP", "soc:U", "y2r:u", "cecd:u" };
    static uint32_t service_handles[] = { 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u };
    if (handle == 1u && command == 0x00050100u) {
        uint32_t name_len = mem32(active_tls_base + 0x8Cu);
        char name[9] = {0};
        for (uint32_t i = 0; i < name_len && i < 8u; ++i)
            name[i] = (char)mem8(active_tls_base + 0x84u + i);
        uint32_t session = 0;
        for (unsigned i = 0; i < sizeof(service_names) / sizeof(service_names[0]); ++i)
            if (strcmp(name, service_names[i]) == 0) { session = service_handles[i]; break; }
        if (!session) {
            fprintf(stderr, "unimplemented SRV:GetServiceHandle service=%s\n", name);
            exit(10);
        }
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x8Cu, session);
        return 0;
    }
    if (handle == 4u) {
        /* err:f is the system error-log service.  Its startup/reporting IPC
           is observational and has no game-state effect in this runner. */
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 1u && command == 0x00010002u) {
        /* srv:RegisterClient -> Result success. */
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 1u && command == 0x00020000u) {
        // SRV:EnableNotification response: Result, copy-handle descriptor,
        // and the process notification semaphore handle.
        put32(active_tls_base + 0x80u, 0x00020040u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0u);
        put32(active_tls_base + 0x8Cu, 2u);
        return 0;
    }
    if (handle == 1u && command == 0x000B0000u) {
        /* SRV's notification poll has no host event source. Report timeout
           so the guest worker returns to its event wait instead of polling
           this absent system subscriber in a hot loop. */
        put32(active_tls_base + 0x80u, 0x000B0040u);
        put32(active_tls_base + 0x84u, 0x09401BFEu);
        if (thread_execution_depth) thread_yield_requested = 1;
        return 0;
    }
    if (handle == 1u && (command == 0x00090040u || command == 0x000A0040u)) {
        /* SRV:Subscribe (0x0009) / Unsubscribe (0x000A). Azahar
           (azahar-master/src/core/hle/service/sm/srv.cpp:198-232) implements
           both as stubs: input word 1 is the notification ID (0x209 on this
           Subscribe from the adventure-log creation path), the response is
           the header echoed with one normal word, Result success. The
           subscription list only feeds the console's notification publisher;
           this standalone runtime has no NS/PTM publisher, and the guest's
           ReceiveNotification poll above already reports timeout, so no
           notification can ever be delivered regardless of subscription
           state. Reaching Subscribe here means the game is wiring up its
           system-notification worker during save-data creation. */
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x00010040u) {
        /* Result, corrected attributes, state, copy descriptor, lock.
           The startup wrapper stores state into its HOME/power flags.
           Leaving it untouched reads stale SRV service-name bytes. */
        uint32_t attributes = mem32(active_tls_base + 0x84u);
        put32(active_tls_base + 0x80u, 0x000100C2u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, attributes);
        put32(active_tls_base + 0x8Cu, 0u);
        put32(active_tls_base + 0x90u, 0u); /* CopyHandleDesc(1) */
        static uint32_t apt_lock_handle;
        if (!apt_lock_handle) {
            if (next_mutex_handle >= 0x10040u) { fputs("native APT mutex table exhausted\n", stderr); exit(16); }
            apt_lock_handle = next_mutex_handle++;
            mutex_live[apt_lock_handle - 0x10000u] = 1u;
            mutex_locked[apt_lock_handle - 0x10000u] = 0u;
        }
        put32(active_tls_base + 0x94u, apt_lock_handle); /* aptSendCommand cmdbuf[5] */
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x00020080u) {
        /* APT Initialize: one Result and a copy descriptor for two events. */
        if (!apt_parameter_event) {
            if (next_event_handle + 2u > 256u) { fputs("APT event table exhausted\n", stderr); exit(22); }
            apt_notification_event = next_event_handle++;
            apt_parameter_event = next_event_handle++;
            event_live[apt_notification_event] = event_live[apt_parameter_event] = 1u;
            event_manual_reset[apt_notification_event] = event_manual_reset[apt_parameter_event] = 1u;
            event_signaled[apt_parameter_event] = apt_pending_signal != 0u;
        }
        put32(active_tls_base + 0x80u, 0x00020043u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x04000000u);
        put32(active_tls_base + 0x8Cu, apt_notification_event);
        put32(active_tls_base + 0x90u, apt_parameter_event);
        return 0;
    }
    if ((handle == 5u || handle == 6u) &&
        (command == 0x00550040u || command == 0x00030040u || command == 0x00040040u ||
         command == 0x00090040u || command == 0x00070000u || command == 0x00080000u ||
         command == 0x00430040u || command == 0x004F0080u ||
         command == 0x00500040u || command == 0x00560000u)) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x000B0040u) {
        /* APT:InquireNotification. Report APTSIGNAL_NONE explicitly; leaving
           this response word untouched leaks stale TLS data into the guest
           and can make it treat random bits as a sleep/HOME event. */
        put32(active_tls_base + 0x80u, 0x000B0080u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) &&
        (command == 0x01010000u || command == 0x01020000u || command == 0x01030000u ||
         command == 0x01040000u)) {
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) &&
        (command == 0x000D0080u || command == 0x000E0080u)) {
        /* Receive consumes a mailbox message; Glance only observes it.
           Azahar MakeBuilder(4,4): Result/sender/signal/size, moved object,
           static-buffer descriptor and pointer (including an empty buffer). */
        uint32_t id = command & 0xFFFF0000u;
        uint32_t output = mem32(active_tls_base + 0x184u);
        uint32_t size = mem32(active_tls_base + 0x88u);
        if (!apt_pending_signal) {
            put32(active_tls_base + 0x80u, id | 0x40u);
            put32(active_tls_base + 0x84u, 0xC8A0CFEFu); /* APT NoData */
            return 0;
        }
        if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc))
            fprintf(stderr, "APT parameter signal=%u consume=%u\n", apt_pending_signal, command == 0x000D0080u);
        put32(active_tls_base + 0x80u, id | 0x104u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x101u);
        put32(active_tls_base + 0x8Cu, apt_pending_signal);
        put32(active_tls_base + 0x90u, 0u);
        put32(active_tls_base + 0x94u, 0x10u);
        put32(active_tls_base + 0x98u, 0u);
        put32(active_tls_base + 0x9Cu, (size << 14) | 2u);
        put32(active_tls_base + 0xA0u, output);
        if (command == 0x000D0080u) {
            apt_pending_signal = 0u;
            if (apt_parameter_event) event_signaled[apt_parameter_event] = 0u;
        }
        return 0;
    }
    if (handle == 9u && (command == 0x00140040u || command == 0x00060040u ||
                         command == 0x00070040u || command == 0x00080040u ||
                         command == 0x00090000u || command == 0x00040002u ||
                         command == 0x00050002u)) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 13u && command == 0x00010082u) {
        /* CFG:GetConfigInfoBlk2. DQ8 asks for block 0x00070001
         * (sound output mode); emulate the USA stereo configuration. */
        uint32_t size = mem32(active_tls_base + 0x84u);
        uint32_t block_id = mem32(active_tls_base + 0x88u);
        uint32_t output = mem32(active_tls_base + 0x90u);
        if (size > 0x1000u || output >= GUEST_RAM_SIZE || size > GUEST_RAM_SIZE - output) {
            fprintf(stderr, "invalid CFG output size=%08X address=%08X\n", size, output);
            exit(10);
        }
        for (uint32_t i = 0; i < size; ++i) put8(output + i, 0u);
        if (size && block_id == 0x00070001u) put8(output, 1u); /* stereo */
        if (size && block_id == 0x000A0002u) put8(output, 1u); /* English, USA executable */
        put32(active_tls_base + 0x80u, 0x00010040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 13u && command == 0x00020000u) {
        /* CFGU_SecureInfoGetRegion. The local executable is the USA release,
           so return region 1 along with the success result. */
        put32(active_tls_base + 0x80u, 0x00020000u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 1u);
        return 0;
    }
    if (handle == 13u && command == 0x00030040u) {
        /* CFGU_GetTransferableId: a console-unique 64-bit id derived from
           the caller's app-id salt (azahar cfg.cpp:526 hashes the
           console-unique-id config block with the salt). No real console id
           exists in this runner, so derive a deterministic value from the
           salt; the guest only stores/compares it. */
        uint32_t salt = mem32(active_tls_base + 0x84u) & 0x000FFFFFu;
        put32(active_tls_base + 0x80u, 0x000300C0u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, salt * 0x9E3779B9u + 0x12345678u);
        put32(active_tls_base + 0x8Cu, (salt ^ 0xA5A5A5A5u) * 0x85EBCA6Bu);
        return 0;
    }
    if (handle == 8u && command == 0x08030204u) {
        uint32_t archive_id = mem32(active_tls_base + 0x88u);
        uint32_t archive_path_type = mem32(active_tls_base + 0x8Cu);
        uint32_t archive_path_size = mem32(active_tls_base + 0x90u);
        uint32_t archive_path_ptr = mem32(active_tls_base + 0xA8u);
        uint32_t file_path_type = mem32(active_tls_base + 0x94u);
        uint32_t file_path_size = mem32(active_tls_base + 0x98u);
        uint32_t file_path_ptr = mem32(active_tls_base + 0xB0u);
        uint32_t memfile = 0u;
        if (archive_id == 3u && file_path_type == 2u && file_path_size == 12u &&
            file_path_ptr < GUEST_RAM_SIZE && mem32(file_path_ptr) == 0u &&
            mem32(file_path_ptr + 4u) == 0u && mem32(file_path_ptr + 8u) == 0u) {
            if (!romfs_image) {
                fprintf(stderr, "RomFS requested without a supplied image\n");
                exit(10);
            }
        }
        if (archive_id == 0x2345678Au && archive_path_type == 2u &&
            archive_path_size == 0x10u && file_path_type == 2u &&
            file_path_size == 0x14u && archive_path_ptr + 0x10u <= GUEST_RAM_SIZE &&
            file_path_ptr + 0x14u <= GUEST_RAM_SIZE) {
            /* azahar archive_ncch.cpp: NCCHArchivePath{u64 tid, u32 media_type,
               u32 unknown}; NCCHFilePath{u32 open_type, u32 content_index,
               u32 filepath_type, char exefs_filepath[8]}.  When the NAND
               system-archive content is missing, NCCHArchive::OpenFile falls
               back per tid: low 0x00010302 (ng_word_list) under high
               0x000400DB (system_data_archive) with filepath_type 0 (RomFS)
               serves BAD_WORD_LIST_DATA - the NG/bad word list the
               name-entry CONFIRM validates the typed name against. */
            uint32_t tid_lo = mem32(archive_path_ptr);
            uint32_t tid_hi = mem32(archive_path_ptr + 4u);
            uint32_t filepath_type = mem32(file_path_ptr + 8u);
            if (tid_hi == 0x000400DBu && tid_lo == 0x00010302u && filepath_type == 0u)
                memfile = 1u;
        }
        put32(active_tls_base + 0x80u, 0x080300C2u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x10u); /* MoveHandleDesc(1) */
        uint32_t file_handle = guest_alloc_ipc_handle();
        if (file_handle < sizeof(guest_file_is_romfs)) {
            guest_file_is_romfs[file_handle] = archive_id == 3u;
            if (memfile) {
                guest_memfile_data[file_handle] = BAD_WORD_LIST_DATA;
                guest_memfile_size[file_handle] = (uint32_t)sizeof(BAD_WORD_LIST_DATA);
                if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc))
                    fprintf(stderr, "FS OpenFileDirectly -> NG word list memfile handle=%08X size=%u\n",
                            file_handle, guest_memfile_size[file_handle]);
            }
        }
        put32(active_tls_base + 0x8Cu, file_handle);
        return 0;
    }
    if (handle == 8u && command == 0x080C00C2u) {
        static uint32_t fake_archive_handle = 0x40u;
        uint32_t archive_id = mem32(active_tls_base + 0x84u);
        put32(active_tls_base + 0x80u, 0x080C0082u); /* MakeBuilder(1,2) */
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x10u); /* MoveHandleDesc(1) */
        uint32_t archive_handle = fake_archive_handle++;
        if (archive_handle < sizeof(guest_archive_id))
            guest_archive_id[archive_handle] = archive_id;
        put32(active_tls_base + 0x8Cu, archive_handle);
        return 0;
    }
    if (handle == 8u && command == 0x080201C2u) {
        uint32_t archive_handle = mem32(active_tls_base + 0x8Cu);
        if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc)) {
            uint32_t path_ptr = mem32(active_tls_base + 0xA4u);
            uint32_t path_size = mem32(active_tls_base + 0x94u);
            fprintf(stderr, "FS OpenFile archive=%08X type=%08X size=%08X path@%08X=",
                    archive_handle, mem32(active_tls_base + 0x90u), path_size, path_ptr);
            for (uint32_t i = 0; i < path_size && i < 128u && path_ptr + i < GUEST_RAM_SIZE; ++i)
                fprintf(stderr, "%02X", mem8(path_ptr + i));
            fputc('\n', stderr);
        }
        uint32_t file_handle = guest_alloc_ipc_handle();
        if (file_handle < sizeof(guest_file_is_romfs))
            guest_file_is_romfs[file_handle] = 0u;
        put32(active_tls_base + 0x80u, 0x080200C2u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x10u); /* MoveHandleDesc(1) */
        put32(active_tls_base + 0x8Cu, file_handle);
        return 0;
    }
    if (handle == 8u && command == 0x080B0102u) {
        /* FS:OpenDirectory. Request words: archive_handle(u64), dirname_type,
           dirname_size, then a static buffer for the path. Return an empty
           directory session so title save-data probes can complete. */
        uint32_t archive_lo = mem32(active_tls_base + 0x84u);
        uint32_t archive_hi = mem32(active_tls_base + 0x88u);
        uint32_t dirname_type = mem32(active_tls_base + 0x8Cu);
        uint32_t dirname_size = mem32(active_tls_base + 0x90u);
        uint32_t dirname_ptr = mem32(active_tls_base + 0x98u);
        uint32_t archive_handle = archive_hi ? archive_hi : archive_lo;
        if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc))
            fprintf(stderr, "FS OpenDirectory archive=%08X/%08X type=%08X size=%08X path@%08X=",
                    archive_lo, archive_hi, dirname_type, dirname_size, dirname_ptr);
        if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc)) {
            for (unsigned i = 0; i < dirname_size && i < 64u && dirname_ptr + i < GUEST_RAM_SIZE; ++i)
                fprintf(stderr, "%02X", mem8(dirname_ptr + i));
            fputc('\n', stderr);
        }
        (void)archive_handle;
        uint32_t dir_handle = guest_alloc_ipc_handle();
        if (dir_handle < sizeof(guest_dir_open))
            guest_dir_open[dir_handle] = 1u;
        put32(active_tls_base + 0x80u, 0x080B0042u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x10u); /* MoveHandleDesc(1) */
        put32(active_tls_base + 0x8Cu, dir_handle);
        return 0;
    }
    if (handle < 256u && guest_dir_open[handle] && (command >> 16) == 0x0801u) {
        /* Directory:Read on the save directory. Report one entry
           "honeySave.bin" so the game treats the save as present and
           skips the first-run creation dialog.
           Request: [count] [mapped-buffer descriptor+addr]. */
        uint32_t count = mem32(active_tls_base + 0x84u);
        uint32_t buffer = mem32(active_tls_base + 0x8Cu);
        if (buffer > GUEST_RAM_SIZE || buffer < 0x1000u) buffer = mem32(active_tls_base + 0x88u);
        if (count > 0x10000u) count = 1u;
        if (buffer >= 0x1000u && buffer + 0x228u <= GUEST_RAM_SIZE) {
            /* FileSys::Entry (0x228 bytes). */
            for (uint32_t i = 0; i < 0x228u; i += 4u) put32(buffer + i, 0u);
            /* UTF-16 filename "honeySave.bin" */
            static const char save_name[] = "honeySave.bin";
            for (unsigned i = 0; save_name[i]; ++i)
                put16(buffer + i * 2u, (uint16_t)(uint8_t)save_name[i]);
            /* 8.3 short name "HONEYSAV" */
            memcpy(ram + buffer + 0x20Cu, "HONEYSAV", 8u);
            ram[buffer + 0x214u] = 0u;
            ram[buffer + 0x215u] = 0x0Au; /* unknown1 */
            memcpy(ram + buffer + 0x216u, "BIN", 3u);
            ram[buffer + 0x219u] = 0u;
            ram[buffer + 0x21Au] = 0x01u; /* unknown2 */
            ram[buffer + 0x21Bu] = 0x00u; /* unknown3 */
            ram[buffer + 0x21Cu] = 0u;    /* is_directory */
            ram[buffer + 0x21Du] = 0u;    /* is_hidden */
            ram[buffer + 0x21Eu] = 0u;    /* is_archive */
            ram[buffer + 0x21Fu] = 1u;    /* is_read_only */
            put32(buffer + 0x220u, 0x00001000u); /* file_size */
            put32(buffer + 0x224u, 0u);
            if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc))
                fprintf(stderr, "Directory:Read -> honeySave.bin buffer=%08X\n", buffer);
            put32(active_tls_base + 0x80u, 0x08010082u);
            put32(active_tls_base + 0x84u, 0u);
            put32(active_tls_base + 0x88u, 1u); /* one entry */
            return 0;
        }
        if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc))
            fprintf(stderr, "Directory:Read -> empty buffer=%08X count=%u\n", buffer, count);
        put32(active_tls_base + 0x80u, 0x08010082u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0u);
        return 0;
    }
    if (handle < 256u && guest_dir_open[handle] && command == 0x08020000u) {
        /* Directory:Close. */
        if (handle < sizeof(guest_dir_open))
            guest_dir_open[handle] = 0u;
        put32(active_tls_base + 0x80u, 0x08020040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 8u && command == 0x08170000u) {
        /* FS:IsSdmcDetected. Report a virtual SD so save-data paths work. */
        put32(active_tls_base + 0x80u, 0x08170080u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 1u);
        return 0;
    }
    if (handle == 8u && command == 0x08180000u) {
        /* FS:IsSdmcWriteable. */
        put32(active_tls_base + 0x80u, 0x08180080u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 1u);
        return 0;
    }
    if (handle == 8u && command == 0x08120040u) {
        /* FS:GetFreeBytes. Report a generous free space. */
        put32(active_tls_base + 0x80u, 0x081200C0u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x04000000u);
        put32(active_tls_base + 0x8Cu, 0u);
        return 0;
    }
    if (handle == 8u && command == 0x08130000u) {
        /* FS:GetCardType. 0 = CTR card. */
        put32(active_tls_base + 0x80u, 0x08130080u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0u);
        return 0;
    }
    if (handle == 8u && command == 0x080F0000u) {
        /* FS:FormatThisUserSaveData. Acknowledge the format. */
        put32(active_tls_base + 0x80u, 0x080F0040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 8u && (command == 0x08090002u || command == 0x08060002u)) {
        /* FS:CreateDirectory / DeleteDirectory. Success. */
        put32(active_tls_base + 0x80u, command == 0x08090002u ? 0x08090040u : 0x08060040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 8u && command == 0x080801C2u) {
        /* FS:CreateFile returns Result only, without an open session. */
        put32(active_tls_base + 0x80u, 0x08080040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 8u && (command == 0x08610042u || command == 0x08620040u || command == 0x08630000u || command == 0x08010002u)) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 8u && command == 0x08490040u) {
        /* FS:GetArchiveResource (azahar fs_user.h:524, fs_user.cpp:1123).
         * Input: media type (ignored by azahar).  Reply: Result + the fixed
         * ArchiveResource azahar returns for every media type
         * (archive.cpp:325): sector 512 B, cluster 16384 B, capacity and free
         * space 0x80000 clusters (8 GiB) - the in-game menu queries this for
         * the save archive right after the START press, and a zero free-space
         * figure would fail its space check.  Response header: command_id
         * echo (bits[31:16]) + normal_params_size 5 (azahar ipc.h Header:
         * bits[11:6] -> 5<<6 = 0x0140). */
        put32(active_tls_base + 0x80u, 0x08490140u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 512u);
        put32(active_tls_base + 0x8Cu, 16384u);
        put32(active_tls_base + 0x90u, 0x80000u);
        put32(active_tls_base + 0x94u, 0x80000u);
        return 0;
    }
    if (handle == 8u && command == 0x080E0080u) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 11u && command == 0x000A0000u) {
        native_hid_get_handles();
        return 0;
    }
    if (handle == 11u && (command == 0x00110000u || command == 0x00120000u ||
                          command == 0x00130000u || command == 0x00140000u)) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 14u && command == 0x001100C2u) {
        /* DSP:LoadComponent. The game has already supplied the signed DSP
         * image through the mapped buffer; this runtime has no DSP core, so
         * acknowledge the component as loaded to let the guest continue into
         * graphics/title initialization. */
        put32(active_tls_base + 0x80u, 0x00110082u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 1u);
        return 0;
    }
    if (handle == 14u && (command == 0x00130082u || command == 0x00140082u)) {
        /* DSP cache maintenance is redundant for this runtime's coherent
           guest RAM, but still validate the range supplied by the title. */
        uint32_t address = mem32(active_tls_base + 0x84u);
        uint32_t size = mem32(active_tls_base + 0x88u);
        if (address > GUEST_RAM_SIZE || size > GUEST_RAM_SIZE - address) {
            fprintf(stderr, "invalid DSP cache range address=%08X size=%08X\n", address, size);
            exit(10);
        }
        put32(active_tls_base + 0x80u, command == 0x00130082u ? 0x00130040u : 0x00140040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 14u && command == 0x00150082u) {
        /* DSP:RegisterInterruptEvents. Audio uses the per-frame Pipe event;
         * other DSP interrupt classes are not yet emulated. */
        if (mem32(active_tls_base + 0x84u) == 2u && mem32(active_tls_base + 0x88u) == 2u) {
            dsp_pipe_interrupt_event = mem32(active_tls_base + 0x90u);
            dsp_source_event_pending = 0;
        }
        put32(active_tls_base + 0x80u, 0x00150040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 14u && command == 0x000D0082u) {
        /* DSP:WriteProcessPipe queues an audio/control packet. Without a DSP
         * core there is no audio to execute, but the IPC itself completes. */
        uint32_t channel = mem32(active_tls_base + 0x84u);
        uint32_t size = mem32(active_tls_base + 0x88u);
        uint32_t buffer = mem32(active_tls_base + 0x90u);
        if (channel == 2u && size == 4u && buffer < GUEST_RAM_SIZE) {
            /* The title's DSP bootstrap waits for a register-0 ACK after its
               initial pipe command. Queue the minimal successful response. */
            dsp_recv_data[0] = 1u;
            dsp_recv_ready[0] = 1u;
        }
        if (channel == 2u && size == 4u && buffer < GUEST_RAM_SIZE &&
            (mem8(buffer) == 0u || mem8(buffer) == 2u)) {
            /* DSP audio-pipe initialization response: 15 DSP-word addresses
             * describing the two shared audio-memory regions. */
            /* This game's parser consumes four leading DSP addresses while
             * building the pipe header, then walks a 15-entry table. Keep
             * the generic Citra prefix and pad the tail so that all 15 table
             * entries are present (the generic fixture only had 11 after
             * the prefix). Zero addresses map to valid, zeroed DSP RAM. */
            static const uint16_t addresses[19] = {
                0xBFFFu, 0x9E92u, 0x8680u, 0xA792u, 0x9430u,
                0x8400u, 0x8540u, 0x9492u, 0x8710u, 0x8410u,
                0xA912u, 0xAA12u, 0xAAD2u, 0xAC52u, 0xAC5Cu,
                0u, 0u, 0u, 0u
            };
            dsp_audio_pipe[0] = 15u;
            dsp_audio_pipe[1] = 0u;
            for (unsigned i = 0; i < 19u; ++i) {
                uint16_t address = addresses[i];
                dsp_audio_pipe[2u + i * 2u] = (uint8_t)address;
                dsp_audio_pipe[3u + i * 2u] = (uint8_t)(address >> 8u);
            }
            dsp_audio_pipe_size = sizeof(dsp_audio_pipe);
            dsp_audio_pipe_pos = 0u;
        }
        if (channel == 2u && size == 4u && buffer < GUEST_RAM_SIZE) {
            /* Audio-pipe state change (Azahar DspPipe::Audio): 0=Initialize,
               1=Shutdown, 2=Wakeup, 3=Sleep.  The DSP only produces frames
               while awake; Initialize/Wakeup also re-serve the table above. */
            uint8_t state = mem8(buffer);
            if (state == 0u || state == 2u) {
                if (!dsp_audio_on && getenv("DQ8_TRACE_AUDIO"))
                    fprintf(stderr, "dsp audio: state change %u -> on\n", state);
                dsp_audio_on = 1;
            } else {
                dsp_audio_on = 0;
            }
        }
        put32(active_tls_base + 0x80u, 0x000D0040u);
        put32(active_tls_base + 0x84u, 0u);
        if (dsp_audio_pipe_size && dsp_pipe_interrupt_event && dsp_pipe_interrupt_event < next_event_handle &&
            event_live[dsp_pipe_interrupt_event]) {
            event_signaled[dsp_pipe_interrupt_event] = 1u;
            guest_notify_handle(dsp_pipe_interrupt_event);
        }
        return 0;
    }
    if (handle == 14u && command == 0x00020040u) {
        /* DSP:RecvDataIsReady reports whether the requested register has a
           queued response from the standalone DSP bootstrap shim. */
        uint32_t reg = mem32(active_tls_base + 0x84u) & 0xFFFFu;
        put32(active_tls_base + 0x80u, 0x00020080u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, reg < 16u ? dsp_recv_ready[reg] : 0u);
        return 0;
    }
    if (handle == 14u && command == 0x00010040u) {
        /* DSP:RecvData consumes the queued bootstrap acknowledgement. */
        uint32_t reg = mem32(active_tls_base + 0x84u) & 0xFFFFu;
        put32(active_tls_base + 0x80u, 0x00010080u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, reg < 16u ? dsp_recv_data[reg] : 0u);
        if (reg < 16u) dsp_recv_ready[reg] = 0u;
        return 0;
    }
    if (handle == 14u && command == 0x001000C0u) {
        /* DSP:ReadPipeIfPossible. The audio pipe returns a length-prefixed
         * list of DSP shared-memory addresses after initialization. */
        uint32_t requested = mem32(active_tls_base + 0x8Cu) & 0xFFFFu;
        uint32_t buffer = mem32(active_tls_base + 0x90u);
        uint32_t available = dsp_audio_pipe_size - dsp_audio_pipe_pos;
        uint32_t actual = requested < available ? requested : available;
        if (buffer < GUEST_RAM_SIZE && actual <= GUEST_RAM_SIZE - buffer)
            for (uint32_t i = 0; i < actual; ++i)
                put8(buffer + i, dsp_audio_pipe[dsp_audio_pipe_pos + i]);
        else actual = 0u;
        dsp_audio_pipe_pos += actual;
        if (dq8_env_on("DQ8_TRACE_IPC", &dq8_env_trace_ipc))
            fprintf(stderr, "DSP pipe read requested=%u actual=%u pos=%u buffer=%08X\n",
                    requested, actual, dsp_audio_pipe_pos, buffer);
        if (dsp_audio_pipe_pos == dsp_audio_pipe_size) {
            dsp_audio_pipe_pos = dsp_audio_pipe_size = 0u;
            if (dsp_pipe_interrupt_event && dsp_pipe_interrupt_event < next_event_handle)
                event_signaled[dsp_pipe_interrupt_event] = 0u;
        }
        put32(active_tls_base + 0x80u, 0x00100082u);
        put32(active_tls_base + 0x84u, 0u);
        /* The guest skips four 16-bit pipe-header words before walking its
         * output table, so the wrapper's loop bound excludes those 8 bytes. */
        uint32_t entries = (actual > 8u) ? (actual - 8u) / 2u : actual;
        put32(active_tls_base + 0x88u, entries);
        /* Its two-stage reader keeps the first read's count in the word
         * immediately before the pipe buffer and later uses that word as the
         * table-loop bound. Refresh it when the payload arrives. */
        if (requested > 2u && actual > 8u && buffer >= 4u)
            put16(buffer - 4u, entries);
        return 0;
    }
    if (handle == 14u && command == 0x000C0040u) {
        /* DSP:ConvertProcessAddressFromDspDram: DSP DRAM is mapped at
         * 0x1FF00000 in the guest address space (libctru OS_DSPRAM_VADDR). */
        uint32_t dsp_address = mem32(active_tls_base + 0x84u);
        /* DSP DRAM addresses are 16-bit words in a region based at
         * process address 0x1FF40000; bit 16 selects the second half. */
        uint32_t process_address = 0x1FF40000u + ((dsp_address & 0x1FFFFu) * 2u);
        put32(active_tls_base + 0x80u, 0x000C0080u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, process_address);
        return 0;
    }
    if (handle == 14u && command == 0x00160000u) {
        /* DSP:GetSemaphoreEventHandle returns a copy of the DSP semaphore
         * event. Keep it unsignaled until DSP semaphore emulation is added. */
        static uint32_t dsp_semaphore_event;
        if (!dsp_semaphore_event) {
            if (next_event_handle >= 256u) { fputs("native guest event table exhausted\n", stderr); exit(22); }
            dsp_semaphore_event = next_event_handle++;
            event_live[dsp_semaphore_event] = 1u;
            event_manual_reset[dsp_semaphore_event] = 1u;
        }
        put32(active_tls_base + 0x80u, 0x00160040u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0x10u); /* copy-handle descriptor */
        put32(active_tls_base + 0x8Cu, dsp_semaphore_event);
        return 0;
    }
    if (handle == 14u && (command == 0x00170040u || command == 0x00070040u ||
                          command == 0x00090040u || command == 0x000A0040u ||
                          command == 0x00120000u || command == 0x00200040u)) {
        /* DSP semaphore/mask and unload operations are acknowledged. The
         * title initialization uses these to start its audio worker. */
        put32(active_tls_base + 0x80u, command == 0x00120000u ? 0x00120000u : command);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 15u && command == 0x00010044u) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 16u) {
        /* y2r:u — the hardware YUV→RGB converter used by the opening movie.
           The conversion itself lives in y2r_start_conversion(); this block
           only implements the command table (mirrors Azahar's y2r_u.cpp). */
        uint16_t y2r_cmd = (uint16_t)(command >> 16);
        uint32_t tls = active_tls_base;
        if (!y2r_inited) y2r_driver_init();
        uint32_t result = 0u;          /* ResultSuccess */
        uint32_t rr[5] = {0, 0, 0, 0, 0};   /* response words after ResultSuccess */
        unsigned normal = 1, translate = 0;
        switch (y2r_cmd) {
        case 0x0001u: y2r.in_fmt = mem32(tls + 0x84u) & 0xFFu; break;
        case 0x0002u: normal = 2; rr[0] = y2r.in_fmt; break;
        case 0x0003u: y2r.out_fmt = mem32(tls + 0x84u) & 0xFFu; break;
        case 0x0004u: normal = 2; rr[0] = y2r.out_fmt; break;
        case 0x0005u: y2r.rotation = mem32(tls + 0x84u) & 0xFFu; break;
        case 0x0006u: normal = 2; rr[0] = y2r.rotation; break;
        case 0x0007u: y2r.align = mem32(tls + 0x84u) & 0xFFu; break;
        case 0x0008u: normal = 2; rr[0] = y2r.align; break;
        case 0x0009u: break;                       /* SetSpacialDithering */
        case 0x000Au: normal = 2; rr[0] = 0u; break; /* GetSpacialDithering: off */
        case 0x000Bu: break;                       /* SetTemporalDithering */
        case 0x000Cu: normal = 2; rr[0] = 0u; break; /* GetTemporalDithering: off */
        case 0x000Du: y2r.irq_enable = mem32(tls + 0x84u) & 1u; break;
        case 0x000Eu: normal = 2; rr[0] = y2r.irq_enable; break;
        case 0x000Fu: /* GetTransferEndEvent: copy one handle to the caller */
            if (!y2r.end_event) {
                if (next_event_handle >= 256u) { fputs("native guest event table exhausted\n", stderr); exit(22); }
                y2r.end_event = next_event_handle++;
                event_live[y2r.end_event] = 1u;
                event_signaled[y2r.end_event] = 0u;
                if (getenv("DQ8_TRACE_Y2R"))
                    fprintf(stderr, "Y2R transfer-end event=%u\n", y2r.end_event);
            }
            put32(tls + 0x80u, (command & 0xFFFF0000u) | (1u << 6) | 2u);
            put32(tls + 0x84u, 0u);
            put32(tls + 0x88u, 0x10u);              /* copy-handle descriptor */
            put32(tls + 0x8Cu, y2r.end_event);
            return 0;
        case 0x0010u: case 0x0011u: case 0x0012u: case 0x0013u: { /* SetSending* */
            struct Y2RBuf *b = (y2r_cmd == 0x0010u) ? &y2r.src_y :
                               (y2r_cmd == 0x0011u) ? &y2r.src_u :
                               (y2r_cmd == 0x0012u) ? &y2r.src_v : &y2r.src_yuyv;
            b->addr = mem32(tls + 0x84u);
            b->size = mem32(tls + 0x88u);
            b->unit = mem32(tls + 0x8Cu) & 0xFFFFu;
            b->gap = mem32(tls + 0x90u) & 0xFFFFu;
            break; }
        case 0x0014u: case 0x0015u: case 0x0016u: case 0x0017u: /* IsFinishedSending* */
            normal = 2; rr[0] = 1u; break;
        case 0x0018u: /* SetReceiving */
            y2r.dst.addr = mem32(tls + 0x84u);
            y2r.dst.size = mem32(tls + 0x88u);
            y2r.dst.unit = mem32(tls + 0x8Cu) & 0xFFFFu;
            y2r.dst.gap = mem32(tls + 0x90u) & 0xFFFFu;
            break;
        case 0x0019u: normal = 2; rr[0] = 1u; break; /* IsFinishedReceiving */
        case 0x001Au: { /* SetInputLineWidth: 1..1024, multiple of 8 */
            uint32_t w = mem32(tls + 0x84u);
            if (!w || w > 1024u || (w % 8u)) result = 0xE0E053FDu;
            else y2r.width = w;
            break; }
        case 0x001Bu: normal = 2; rr[0] = y2r.width; break;
        case 0x001Cu: { /* SetInputLines: 1..1024 */
            uint32_t l = mem32(tls + 0x84u);
            if (!l || l > 1024u) result = 0xE0E053FDu;
            else y2r.lines = l;
            break; }
        case 0x001Du: normal = 2; rr[0] = y2r.lines; break;
        case 0x001Eu: { /* SetCoefficient: 8 s16 words packed little-endian */
            for (unsigned i = 0; i < 4u; ++i) {
                uint32_t w = mem32(tls + 0x84u + i * 4u);
                y2r.coef[2u * i] = (int32_t)(int16_t)(w & 0xFFFFu);
                y2r.coef[2u * i + 1u] = (int32_t)(int16_t)(w >> 16);
            }
            break; }
        case 0x001Fu: /* GetCoefficient */
            normal = 5;
            for (unsigned i = 0; i < 4u; ++i) {
                uint32_t lo = (uint32_t)(int16_t)y2r.coef[2u * i];
                uint32_t hi = (uint32_t)(int16_t)y2r.coef[2u * i + 1u];
                rr[i] = lo | (hi << 16);
            }
            break;
        case 0x0020u: { /* SetStandardCoefficient */
            uint32_t idx = mem32(tls + 0x84u);
            if (idx >= 4u) result = 0xE0E053EDu;
            else for (unsigned i = 0; i < 8u; ++i) y2r.coef[i] = y2r_std_coef[idx][i];
            break; }
        case 0x0021u: { /* GetStandardCoefficient */
            uint32_t idx = mem32(tls + 0x84u);
            if (idx >= 4u) { result = 0xE0E053EDu; break; }
            normal = 5;
            for (unsigned i = 0; i < 4u; ++i) {
                uint32_t lo = (uint32_t)(int16_t)y2r_std_coef[idx][2u * i];
                uint32_t hi = (uint32_t)(int16_t)y2r_std_coef[idx][2u * i + 1u];
                rr[i] = lo | (hi << 16);
            }
            break; }
        case 0x0022u: y2r.alpha = mem32(tls + 0x84u) & 0xFFFFu; break;
        case 0x0023u: normal = 2; rr[0] = y2r.alpha; break;
        case 0x0024u: break;  /* SetDitheringWeightParams: accepted, not modelled */
        case 0x0025u: normal = 9; break;  /* GetDitheringWeightParams: zeros */
        case 0x0026u: y2r_start_conversion(); break;  /* StartConversion */
        case 0x0027u: break;  /* StopConversion */
        case 0x0028u: normal = 2; rr[0] = 0u; break;  /* IsBusyConversion: done */
        case 0x0029u: { /* SetPackageParameter: 12 packed bytes */
            uint32_t w0 = mem32(tls + 0x84u), w1 = mem32(tls + 0x88u), w2 = mem32(tls + 0x8Cu);
            uint32_t idx = w2 & 0xFFu;
            y2r.in_fmt = w0 & 0xFFu;
            y2r.out_fmt = (w0 >> 8) & 0xFFu;
            y2r.rotation = (w0 >> 16) & 0xFFu;
            y2r.align = (w0 >> 24) & 0xFFu;
            if (!(w1 & 0xFFFFu) || (w1 & 0xFFFFu) > 1024u || ((w1 & 0xFFFFu) % 8u)) result = 0xE0E053FDu;
            else if (!(w1 >> 16) || (w1 >> 16) > 1024u) result = 0xE0E053FDu;
            else {
                y2r.width = w1 & 0xFFFFu;
                y2r.lines = (w1 >> 16) & 0xFFFFu;
            }
            if (!result) {
                if (idx >= 4u) result = 0xE0E053EDu;
                else for (unsigned i = 0; i < 8u; ++i) y2r.coef[i] = y2r_std_coef[idx][i];
            }
            if (!result) y2r.alpha = (w2 >> 16) & 0xFFFFu;
            break; }
        case 0x002Au: normal = 2; rr[0] = 0u; break;  /* PingProcess */
        case 0x002Bu: y2r_driver_init(); break;        /* DriverInitialize */
        case 0x002Cu: break;                           /* DriverFinalize */
        default:
            if (getenv("DQ8_TRACE_Y2R"))
                fprintf(stderr, "Y2R unhandled command %04X (acked)\n", y2r_cmd);
            break;
        }
        put32(tls + 0x80u, (command & 0xFFFF0000u) | (normal << 6) | translate);
        put32(tls + 0x84u, result);
        for (unsigned i = 0; i + 1u < normal; ++i)
            put32(tls + 0x88u + i * 4u, i < 5u ? rr[i] : 0u);
        return 0;
    }

    if (handle == 7u && command == 0x00160042u) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 7u && command == 0x00130042u) {
        uint32_t relay = mem32(active_tls_base + 0x8Cu);
        /* Service handles occupy the same small integers as early events.
           A value that collides with a known service session is not the
           guest's GSP relay event; fall back to the newest created event. */
        if (!relay || relay == 7u || relay == 8u || relay == 11u || relay == 14u || relay == 15u)
            relay = (next_event_handle > 0x20u) ? next_event_handle - 1u : relay;
        put32(active_tls_base + 0x80u, 0x00130082u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 1u);       /* interrupt thread id */
        put32(active_tls_base + 0x8Cu, 0x10u);    /* copy-handle descriptor */
        put32(active_tls_base + 0x90u, 0x30020u); /* synthetic GSP shared memory */
        gsp_relay_event = relay;
        if (relay && relay < 256u) {
            event_live[relay] = 1u;
            if (!event_manual_reset[relay]) event_signaled[relay] = 0;
        }
        gsp_thread_id = 1u;
        gsp_relay_registered = 1u;
        if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait))
            fprintf(stderr, "GSP relay registration event=%08X live=%u next_event=%08X arg1=%08X arg2=%08X arg3=%08X\n",
                    relay, relay < 256u ? event_live[relay] : 0u, next_event_handle,
                    mem32(active_tls_base + 0x84u), mem32(active_tls_base + 0x88u), mem32(active_tls_base + 0x90u));
        return 0;
    }
    if (handle == 7u && command == 0x000B0040u) {
        gsp_lcd_force_black = mem32(active_tls_base + 0x84u) ? 1u : 0u;
        put32(active_tls_base + 0x84u, 0u);
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
            fprintf(stderr, "GSP SetLcdForceBlack enabled=%u\n", gsp_lcd_force_black);
        return 0;
    }
    if (handle == 7u && command == 0x001F0082u) {
        uint32_t address = mem32(active_tls_base + 0x84u);
        uint32_t size = mem32(active_tls_base + 0x88u);
        /* StoreDataCache (0x1F) is a no-op for this runtime's coherent host
           RAM, but still checks that the requested guest range is addressable. */
        uint32_t result = (address <= GUEST_RAM_SIZE && size <= GUEST_RAM_SIZE - address)
            ? 0u : 0xE0E02BF5u;
        put32(active_tls_base + 0x84u, result);
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
            fprintf(stderr, "GSP StoreDataCache address=%08X size=%08X result=%08X\n", address, size, result);
        return 0;
    }
    if (handle == 7u && command == 0x000C0000u) {
        ++gsp_queue_calls; /* t_2d934e23: one guest logic/render tick */
        uint32_t queue = gsp_shared_base + 0x800u + gsp_thread_id * 0x200u;
        uint32_t header = mem32(queue);
        unsigned index = header & 0xFFu, count = (header >> 8) & 0xFFu;
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
            fprintf(stderr, "GSP TriggerCmdReqQueue base=%08X tid=%u queue=%08X header=%08X index=%u count=%u\n",
                    gsp_shared_base, gsp_thread_id, queue, header, index, count);
        for (unsigned i = 0; i < count && i < 15u; ++i) {
            uint32_t slot = queue + 0x20u + ((index + i) % 15u) * 0x20u;
            uint32_t cmd = mem32(slot);
            if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
                fprintf(stderr, "GSP command slot=%u words=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X\n",
                        i, cmd, mem32(slot + 4u), mem32(slot + 8u), mem32(slot + 12u),
                        mem32(slot + 16u), mem32(slot + 20u), mem32(slot + 24u), mem32(slot + 28u));
            if ((cmd & 0xFFu) == 1u && dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu)) {
                uint32_t list = mem32(slot + 4u), size = mem32(slot + 8u);
                fprintf(stderr, "PICA list addr=%08X bytes=%u first_words=", list, size);
                for (unsigned j = 0; j < 32u && j * 4u < size; ++j)
                    fprintf(stderr, "%08X%s", mem32(list + j * 4u), j == 31u ? "\n" : "/");
            }
            if ((cmd & 0xFFu) == 1u) {
                uint32_t list = mem32(slot + 4u), size = mem32(slot + 8u);
                pica_process_command_list(list, size);
                const char *dump_path = getenv("DQ8_DUMP_PICA");
                uint32_t mapped_list;
                if (dump_path && guest_memory_range(list, size, &mapped_list)) {
                    FILE *dump = fopen(dump_path, "wb");
                    if (dump) {
                        size_t written = fwrite(ram + mapped_list, 1u, size, dump);
                        fclose(dump);
                        fprintf(stderr, "PICA command-list dump: %zu/%u bytes -> %s\n", written, size, dump_path);
                    }
                }
                const char *dump_prefix = getenv("DQ8_DUMP_PICA_ALL");
                if (dump_prefix && guest_memory_range(list, size, &mapped_list)) {
                    char numbered_path[1024];
                    snprintf(numbered_path, sizeof(numbered_path), "%s-%04u-%08X-%u.bin",
                             dump_prefix, ++gsp_pica_dump_index, list, size);
                    FILE *dump = fopen(numbered_path, "wb");
                    if (dump) {
                        size_t written = fwrite(ram + mapped_list, 1u, size, dump);
                        fclose(dump);
                        fprintf(stderr, "PICA command-list capture: %zu/%u bytes -> %s\n",
                                written, size, numbered_path);
                    } else {
                        fprintf(stderr, "cannot write PICA command-list capture: %s\n", numbered_path);
                    }
                }
            } else if ((cmd & 0xFFu) == 2u) {
                uint32_t args[7];
                for (unsigned j = 0; j < 7u; ++j) args[j] = mem32(slot + 4u + j * 4u);
                gsp_memory_fill(args);
            } else if ((cmd & 0xFFu) == 3u) {
                uint32_t args[7];
                for (unsigned j = 0; j < 7u; ++j) args[j] = mem32(slot + 4u + j * 4u);
                gsp_display_transfer(args);
            } else if ((cmd & 0xFFu) == 0u) {
                uint32_t source = mem32(slot + 4u), dest = mem32(slot + 8u), size = mem32(slot + 12u);
                uint32_t mapped_source, mapped_dest;
                pica_gl_flush_guest_rt(); /* DMA reads source bytes from guest RAM */
                if (gsp_dma_log_n < 8u) {
                    gsp_dma_log[gsp_dma_log_n][0] = source;
                    gsp_dma_log[gsp_dma_log_n][1] = dest;
                    gsp_dma_log[gsp_dma_log_n][2] = size;
                    gsp_dma_log_n++;
                } else {
                    memmove(gsp_dma_log, gsp_dma_log + 1, sizeof(gsp_dma_log) - sizeof(gsp_dma_log[0]));
                    gsp_dma_log[7][0] = source;
                    gsp_dma_log[7][1] = dest;
                    gsp_dma_log[7][2] = size;
                }
                if (guest_memory_range(source, size, &mapped_source) &&
                    guest_memory_range(dest, size, &mapped_dest))
                    memmove(ram + mapped_dest, ram + mapped_source, size);
                gsp_enqueue_interrupt(6u); /* DMA complete */
            }
            /* Cache management and command-list submissions are completed in
               queue order. P3D is emitted only by an actual GPU backend. */
        }
        if (count) {
            uint32_t next_index = (index + count) % 15u;
            uint32_t next_header = (header & 0xFFFF0000u) | next_index;
            put32(queue, next_header);
        }
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if (handle == 7u && (command == 0x000C0000u || command == 0x00140000u ||
                         command == 0x00170000u || command == 0x00180000u ||
                         command == 0x001C0040u || command == 0x001E0080u ||
                         command == 0x000A0044u || command == 0x00060082u ||
                         command == 0x00080082u || command == 0x00090082u)) {
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x004B00C2u) {
        /* APT_AppletUtility(4..7) probes the shell lid or changes an APT
           transition lock. This standalone runtime has no shell or applet
           transition to coordinate, so return the open/unlocked state. */
        uint32_t utility_id = mem32(active_tls_base + 0x84u);
        uint32_t input_size = mem32(active_tls_base + 0x88u);
        uint32_t output_size = mem32(active_tls_base + 0x8Cu);
        uint32_t output_desc = mem32(active_tls_base + 0x180u);
        uint32_t output = mem32(active_tls_base + 0x184u);
        int valid_utility =
            (utility_id == 4u && input_size == 1u && output_size == 1u) ||
            ((utility_id == 5u || utility_id == 7u) && input_size == (utility_id == 5u ? 8u : 4u) && output_size == 1u) ||
            (utility_id == 6u && input_size == 4u && output_size == 4u);
        if (!valid_utility || output > GUEST_RAM_SIZE || output_size > GUEST_RAM_SIZE - output) {
            fprintf(stderr, "unsupported APT AppletUtility id=%u in=%u out=%u desc=%08X ptr=%08X\n",
                    utility_id, input_size, output_size, output_desc, output);
            exit(10);
        }
        /* Match Azahar: utilities default to zero; only TryLockTransition
           (6) returns true. Other queries must not invent positive state. */
        for (uint32_t i = 0; i < output_size; ++i) put8(output + i, 0u);
        if (utility_id == 6u && output_size) put8(output, 1u);
        put32(active_tls_base + 0x80u, 0x004B0082u);
        put32(active_tls_base + 0x84u, 0u); /* IPC Result */
        put32(active_tls_base + 0x88u, 0u); /* utility Result */
        put32(active_tls_base + 0x8Cu, (output_size << 14) | 2u);
        put32(active_tls_base + 0x90u, output);
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x003E0080u) {
        /* APT:ReplySleepQuery. This standalone runtime has no system sleep
           transition to coordinate, so acknowledge the guest's reply. */
        put32(active_tls_base + 0x80u, 0x003E0040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x003B0040u) {
        /* APT:CancelLibraryApplet; no system library applet is running. */
        put32(active_tls_base + 0x80u, 0x003B0040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x00050040u) {
        /* APT:GetAppletManInfo for a single current application with the
           normal HOME Menu registered as the active menu applet. */
        put32(active_tls_base + 0x80u, 0x00050140u);
        put32(active_tls_base + 0x84u, 0u);
        put32(active_tls_base + 0x88u, 0u);     /* applet position */
        put32(active_tls_base + 0x8Cu, 0u);     /* requested applet */
        put32(active_tls_base + 0x90u, 0x101u); /* HOME Menu */
        put32(active_tls_base + 0x94u, 0x300u); /* current application */
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x002B0000u) {
        /* APT:PrepareToJumpToHomeMenu; the host runtime has no HOME Menu
           process to prepare, so leave the current guest active. */
        put32(active_tls_base + 0x80u, 0x002B0040u);
        put32(active_tls_base + 0x84u, 0u);
        return 0;
    }
    if ((handle == 5u || handle == 6u) && command == 0x002C0044u) {
        /* Headless HOME round trip. LeaveHomeMenu resumes the application
           with WakeupByPause (0xB), not Response (3). Signal APT's parameter
           event, without editing guest objects or manufacturing HID input. */
        put32(active_tls_base + 0x80u, 0x002C0040u);
        put32(active_tls_base + 0x84u, 0u);
        apt_pending_signal = 0xBu;
        if (apt_parameter_event) {
            event_signaled[apt_parameter_event] = 1u;
            guest_notify_handle(apt_parameter_event);
        }
        return 0;
    }
    fprintf(stderr, "unimplemented 3DS SVC SendSyncRequest: handle=0x%08X command=0x%08X ipc=%08X pc=%08X lr=%08X words=%08X/%08X/%08X/%08X/%08X/%08X\n",
            handle, command, active_tls_base + 0x80u, current_guest_pc,
            active_guest_regs ? active_guest_regs[14] : 0u,
            mem32(active_tls_base + 0x84u),
            mem32(active_tls_base + 0x88u), mem32(active_tls_base + 0x8Cu),
            mem32(active_tls_base + 0x90u), mem32(active_tls_base + 0x94u),
            mem32(active_tls_base + 0x98u));
    exit(10);
}
static uint32_t svc_connect_to_port(uint32_t *r) {
    uint32_t address = r[1];
    char name[9] = {0};
    for (unsigned i = 0; i < 8; ++i) {
        if (address + i >= GUEST_RAM_SIZE) break;
        name[i] = (char)mem8(address + i);
        if (!name[i]) break;
    }
    if (strcmp(name, "srv:") == 0) {
        r[1] = 1u;
        return 0;
    }
    if (strcmp(name, "err:f") == 0) {
        r[1] = 4u;
        return 0;
    }
    fprintf(stderr, "unimplemented 3DS SVC ConnectToPort: name='%s'\n", name);
    exit(14);
}
static uint32_t svc_close_handle(uint32_t handle) {
    if (handle == 1u || handle == 2u || handle == 4u || handle == 0x30000u || handle == 0x40000u) return 0;
    /* Static service sessions srv:GetServiceHandle hands out are 5..17
       (see service_handles in svc_send_sync: ... y2r:u=16, cecd:u=17); the
       previous <=16 bound missed cecd:u, so the save-menu flow's legitimate
       CloseHandle(0x11) hit the unimplemented tripwire and killed the run. */
    if ((handle >= 5u && handle <= 17u) || (handle >= 0x20u && handle < 0x100u) ||
        (handle >= 0x30020u && handle < 0x30100u)) return 0; /* synthetic sessions */
    if (handle >= 0x50000u && handle < 0x50040u) return 0; /* guest thread handles */
    if (handle >= 0x20000u && handle < next_arbiter_handle &&
        arbiter_live[handle - 0x20000u]) {
        arbiter_live[handle - 0x20000u] = 0;
        return 0;
    }
    if (handle < next_event_handle && event_live[handle]) {
        event_live[handle] = 0;
        event_signaled[handle] = 0;
        return 0;
    }
    if (handle >= 0x10000u && handle < next_mutex_handle && mutex_live[handle - 0x10000u]) {
        mutex_live[handle - 0x10000u] = 0;
        return 0;
    }
    fprintf(stderr, "unimplemented 3DS SVC CloseHandle: handle=%08X\n", handle);
    exit(15);
}
static uint32_t control_memory_next = 0x14000000u;
static uint32_t svc_control_memory(uint32_t *r) {
    /* SVC #1 returns the resulting address in r1 and its Result in r0. */
    uint32_t operation = r[0], requested = r[1], size = r[3];
    fprintf(stderr, "ControlMemory pc=%08X lr=%08X op=%08X req=%08X addr1=%08X size=%08X perm=%08X\n", current_guest_pc, r[14], operation, requested, r[2], size, r[4]);
    uint32_t result_address = requested;
    if (operation == 0x10003u && requested == 0u) {
        result_address = (control_memory_next + 0xFFFu) & ~0xFFFu;
        uint32_t amount = (size + 0xFFFu) & ~0xFFFu;
        if (amount && result_address <= GUEST_RAM_SIZE - amount) {
            control_memory_next = result_address + amount;
        }
    }
    if (result_address >= GUEST_RAM_SIZE) return 0xD8E007F7u;
    r[1] = result_address;
    r[0] = 0;
    return 0;
}
static uint32_t svc_get_process_info(uint32_t *r) {
    uint32_t process = r[1], type = r[2];
    if (process != 0xFFFF8001u && process != 0xFFFF8000u && process != 1u && process != 0x40000u) {
        fprintf(stderr, "SVC GetProcessInfo: unsupported process handle=%08X type=%08X\n", process, type);
        exit(17);
    }
    uint64_t value = 0;
    switch (type) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: value = 0; break;
    case 0x10002: value = 0x43BCB4u; break;
    case 0x10003: case 0x10004: value = 0; break;
    case 0x10005: value = 0x00100000u; break;
    case 0x10006: value = 0x0053BCB4u; break;
    case 0x10007: value = 0x0053BCB4u; break;
    case 20: value = 0x0C000000u; break; /* 0x20000000 - O3DS linear VA base (0x14000000). */
    default:
        fprintf(stderr, "SVC GetProcessInfo: unsupported type=%08X process=%08X\n", type, process);
        exit(17);
    }
    fprintf(stderr, "GetProcessInfo pc=%08X type=%08X -> %08llX\n", current_guest_pc, type, (unsigned long long)value);
    /* The guest ARM SVC wrapper stores the 64-bit result through its saved
       output pointer. The kernel ABI returns the value in r1:r2. */
    r[1] = (uint32_t)value;
    r[2] = (uint32_t)(value >> 32);
    return 0;
}
static void svc_get_system_tick(uint32_t *r) {
    struct timespec now;
    uint64_t ticks;
    if (dq8_det_mode) {
        /* DETERMINISTIC (t_115d297c): read the virtual clock so the same
           instruction budget returns the same tick on every run.  Same
           268.111856 MHz tick rate as the hardware. */
        uint64_t ns = dq8_guest_now_ns();
        ticks = ns / 1000000000ull * 268111856ull +
                ns % 1000000000ull * 268111856ull / 1000000000ull;
        tempo_tick_seen(ticks, current_guest_pc);
        r[0] = (uint32_t)ticks;
        r[1] = (uint32_t)(ticks >> 32);
        return;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now)) { perror("clock_gettime"); exit(25); }
    ticks = (uint64_t)now.tv_sec * 268111856ull + (uint64_t)now.tv_nsec * 268111856ull / 1000000000ull;
    tempo_tick_seen(ticks, current_guest_pc);
    r[0] = (uint32_t)ticks;
    r[1] = (uint32_t)(ticks >> 32);
}
static uint32_t svc_duplicate_handle(uint32_t handle) {
    /* Duplicate the current-process pseudo-handle into a stable ordinary
       process handle. Other startup handles remain identity aliases. */
    if (handle == 0xFFFF8000u) return 0x40000u;
    if (handle == 0xFFFF8001u || handle == 0xFFFF8002u ||
        handle == 1u || handle == 2u || handle == 4u ||
        handle == 0x40000u ||
        (handle >= 0x10000u && handle < next_mutex_handle && mutex_live[handle - 0x10000u]) ||
        (handle >= 3u && handle < next_event_handle && event_live[handle])) return handle;
    fprintf(stderr, "SVC DuplicateHandle: unsupported handle=%08X\n", handle);
    exit(27);
}
/* The title's memory-pool bootstrap links one 0x44-byte page record per
   guest page. Its translated inner loop is instruction-dispatch bound and can
   take minutes for multi-million-page pools. This fast path is limited to the
   exact linked-list loop at 0x00119198, only on the main guest context, and
   performs the same writes and leaves the same loop-exit state. */
static int guest_fast_page_list(uint32_t *r, uint32_t *pc,
                                int *n, int *z, int *c, int *v,
                                uint64_t *steps) {
    if (*pc != 0x00119198u || thread_execution_depth || r[6] != 0u ||
        r[5] <= r[3] || r[5] > 0x00800000u || r[4] > GUEST_RAM_SIZE - 0x18u)
        return 0;
    uint32_t first = r[3];
    uint64_t last = (uint64_t)r[1] + ((uint64_t)r[5] - 1u) * 0x44u;
    if (r[1] >= GUEST_RAM_SIZE || last > GUEST_RAM_SIZE - 12u) return 0;
    uint32_t head_field = r[4] + 0x10u, tail_field = r[4] + 0x14u;
    uint32_t tail = mem32(tail_field);
    if (tail && tail > GUEST_RAM_SIZE - 8u) return 0;
    uint32_t previous_tail = tail;
    for (uint32_t i = first; i < r[5]; ++i) {
        uint32_t node = r[1] + i * 0x44u;
        if (tail) {
            put32(tail + 4u, node);
            put32(node + 8u, tail);
            put32(tail_field, node);
            put32(node + 4u, 0u);
        } else {
            put32(tail_field, node);
            put32(head_field, node);
            put32(node + 4u, 0u);
            put32(node + 8u, 0u);
        }
        previous_tail = tail;
        tail = node;
    }
    r[12] = previous_tail;
    r[3] = r[5];
    r[0] = (uint32_t)last;
    r[2] = head_field;
    *n = 0; *z = 1; *c = 1; *v = 0;
    *pc = 0x00119224u;
    *steps += ((uint64_t)r[5] - first) * 15u;
    return 1;
}
static void guest_stack_release(unsigned slot);
static void svc_exit_thread(void) {
    /* Audit R2-03: thread termination releases every kernel mutex the
       thread still owns, the way the 3DS kernel's ReleaseThreadMutexes
       does.  The release goes through the same readiness -> notify ->
       acquire hand-off every producer uses, so recursive holders unwind
       fully and parked Wait1/WaitN waiters compete for the freed mutex
       exactly as they would after an explicit release. */
    uint32_t my_handle = current_guest_thread_id;
    for (unsigned slot = 0; slot < 64u; ++slot) {
        if (mutex_live[slot] && mutex_locked[slot] && mutex_owner[slot] == my_handle) {
            mutex_recursion[slot] = 0;
            mutex_locked[slot] = 0;
            mutex_owner[slot] = 0;
            guest_notify_handle(0x10000u + slot);
        }
    }
    /* Wake every thread joined on this one (WaitSynchronization on a thread
       handle blocks until the target exits).  The thread object only becomes
       readable when `active` clears, which happens after this SVC returns, so
       readiness is satisfied here directly; acquiring a dead thread object
       has no side effect, so every single-object joiner completes with
       success and a recorded result. */
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        GuestThread *t = &guest_threads[i];
        if (guest_thread_waits_on(t, my_handle) && !t->wait_n_active)
            guest_wait_finish(t, 0u, 0u, 0);
    }
    /* Release the stack-extent slot: a later thread that reuses this
       stack address must not be reported as a live overlap.  Without
       this, slots of finished threads stay 'live' forever and the
       overlap check fires on every frame (t_eb3aef76 follow-up). */
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        if (guest_threads[i].guest_thread_id == my_handle) {
            guest_stack_release(i);
            break;
        }
    }
    /* The currently executing guest thread returns to its creator's scheduler. */
}
static void svc_exit_process(void) {
    fputs("3DS SVC ExitProcess reached\n", stderr);
    exit(0);
}
static int guest_threads_enabled(void) {
    /* t_275eb521: called from the scheduler/svc paths; resolve once per
       process (the value cannot change at run time). */
    static int cached = -1;
    if (cached < 0) {
        const char *value = getenv("DQ8_RUN_GUEST_THREADS");
        cached = value && *value && strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
    }
    return cached;
}
/* Reserved stack size per guest thread.  The CreateThread ABI passes only the
   stack *top* (r[3]); the base is the allocator's block.  We conservatively
   assume the guest's default thread-stack block (64 KiB) when computing the
   downward extent so two threads whose tops sit within one block of each other
   are flagged as colliding (e.g. 0x0868C710 vs 0x0868C6D8, 56 bytes apart). */
#define GUEST_STACK_BLOCK 0x10000u
/* Guest thread-stack extents (t_eb3aef76): the 3DS kernel guarantees that any
   two live threads of the process have disjoint stack regions.  Record each
   thread's [low, high) from the ABI stack top (r[3]) down to the lowest SP the
   thread actually reaches, and refuse (loudly) a CreateThread whose implied
   extent collides with a still-live peer.  An overlap here is a *runtime
   memory-layout* defect: the guest allocator handed the same block to a thread
   stack and to a heap record, which is exactly the frame-stomp/aliasing class
   that produced the 0x005CB6E0 escapes, the round-11 NULL-call (pc=0) and the
   operator S1 node-walk fault.  Stack size is unknown from the ABI (only the
   top is passed), so the invariant is conservative: two threads may not share
   the same stack *page*, which the retail allocator never does. */
#define GUEST_THREAD_STACK_SLOTS 64u
static uint32_t guest_stack_top[GUEST_THREAD_STACK_SLOTS];
static uint32_t guest_stack_low[GUEST_THREAD_STACK_SLOTS];   /* lowest SP seen */
static int guest_stack_live[GUEST_THREAD_STACK_SLOTS];
static int guest_stack_overlaps;
static int guest_stack_relocated;             /* count of relocated threads */
static int guest_stack_no_relocate;           /* env DQ8_STACK_NO_RELOCATE=1 */
/* Relocation region for colliding thread stacks (t_eb3aef76 layout fix):
   [0x10000000, 0x10400000) — 64 private 64 KiB slots, clear of the app heap
   (top ~0x08900000), LINEAR (0x14000000), TLS (0x1FF82000) and inside
   GUEST_RAM_SIZE.  A colliding stack top is moved to the next free slot here,
   giving every live thread a disjoint extent and removing the aliasing that
   let a heap record / peer frame stomp another thread's saved lr. */
#define GUEST_RELOC_BASE  0x10000000u
#define GUEST_RELOC_SLOTS 64u
static uint32_t guest_reloc_next = GUEST_RELOC_BASE;
static int guest_stack_collides(uint32_t top, uint32_t block) {
    uint32_t lo = top - block, hi = top;
    for (unsigned j = 0; j < GUEST_THREAD_STACK_SLOTS; ++j) {
        if (!guest_stack_live[j]) continue;
        if (lo < guest_stack_top[j] && guest_stack_low[j] < hi) return 1;
    }
    return 0;
}
/* Allocator-owned regions (mirrors svc_control_memory's two reservations): a
   stack TOP inside one of these is guest-allocator output whose liveness the
   runtime cannot know — the round-9 mechanism, where a live ~40-byte record at
   0x08690358..88 was handed out as a thread stack and the serializer's memcpy
   stomped the frames on top of it (thread-vs-thread checks cannot see a heap
   buffer).  Stacks in these regions are therefore relocated UNCONDITIONALLY;
   statically placed .bss stacks are left alone (peer-collision still applies). */
#define GUEST_APP_HEAP_LO  0x08000000u
#define GUEST_APP_HEAP_HI  0x09300000u
#define GUEST_LINEAR_LO    0x14000000u
#define GUEST_LINEAR_HI    0x16D00000u
static int guest_stack_in_allocator_region(uint32_t top) {
    return (top >= GUEST_APP_HEAP_LO && top < GUEST_APP_HEAP_HI) ||
           (top >= GUEST_LINEAR_LO && top < GUEST_LINEAR_HI);
}
/* DQ8_STACK_NO_RELOCATE: disable the stack-relocation layout fix (isolation /
   A-B against the raw allocator behaviour).  Read once on first CreateThread. */
static void guest_stack_env_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *value = getenv("DQ8_STACK_NO_RELOCATE");
    guest_stack_no_relocate = (value && *value && *value != '0') ? 1 : 0;
    if (guest_stack_no_relocate)
        fprintf(stderr, "DQ8_STACK_NO_RELOCATE=1: colliding thread stacks are NOT relocated\n");
}
/* Mark a thread slot dead so a recycled stack block can be handed to a new
   thread without a false-positive overlap against the finished thread. */
static void guest_stack_release(unsigned slot) {
    if (slot < GUEST_THREAD_STACK_SLOTS) guest_stack_live[slot] = 0;
}
/* Grow a thread's recorded low-water SP; returns 1 if the extent now collides
   with another live thread's extent (the aliasing class we must catch). */
static int guest_stack_track(unsigned slot, uint32_t sp) {
    if (slot >= GUEST_THREAD_STACK_SLOTS) return 0;
    uint32_t page = sp & ~0xFFFu;
    if (!guest_stack_live[slot] || page < guest_stack_low[slot]) {
        guest_stack_low[slot] = page;
        guest_stack_live[slot] = 1;
    }
    /* A relocated thread must stay inside its private 64 KiB slot: crossing
       below the zone would land in the main thread's crt stack (untracked,
       so pairwise extent checks cannot see it).  Warn once per slot. */
    {
        static uint64_t reloc_overflow_warned;
        if (page < GUEST_RELOC_BASE &&
            guest_stack_top[slot] > GUEST_RELOC_BASE &&
            guest_stack_top[slot] <=
                GUEST_RELOC_BASE + GUEST_RELOC_SLOTS * GUEST_STACK_BLOCK &&
            !(reloc_overflow_warned & (1ull << slot))) {
            reloc_overflow_warned |= 1ull << slot;
            fprintf(stderr,
                    "guest relocated stack OVERFLOW: slot %u low=0x%08X "
                    "below reloc zone base 0x%08X pc=%08X\n",
                    slot, page, (unsigned)GUEST_RELOC_BASE, current_guest_pc);
        }
    }
    for (unsigned j = 0; j < GUEST_THREAD_STACK_SLOTS; ++j) {
        if (j == slot || !guest_stack_live[j]) continue;
        uint32_t a_lo = guest_stack_low[slot], a_hi = guest_stack_top[slot];
        uint32_t b_lo = guest_stack_low[j], b_hi = guest_stack_top[j];
        if (a_lo < b_hi && b_lo < a_hi) {
            static uint64_t overlap_warned;
            unsigned pair = (slot < j ? slot : j) * 64u + (slot < j ? j : slot);
            if (!(overlap_warned & (1ull << (pair & 63u)))) {
                overlap_warned |= 1ull << (pair & 63u);
            } else {
                return 1;
            }
            fprintf(stderr,
                    "guest thread-stack OVERLAP: slot %u [0x%08X,0x%08X) vs slot %u "
                    "[0x%08X,0x%08X) pc=%08X (aliasing class: stack/heap share a block)\n",
                    slot, a_lo, a_hi, j, b_lo, b_hi, current_guest_pc);
            return 1;
        }
    }
    return 0;
}
static uint32_t svc_create_thread(uint32_t *r) {
    /* 3DS kernel ABI: priority, entry, argument, stack top, processor id;
       the syscall returns its handle in r1. Run startup workers synchronously
       until ExitThread so their initialization is visible to the main guest. */
    if (next_guest_thread_handle == 0x50040u || r[1] == 0u || r[3] >= GUEST_RAM_SIZE) {
        fprintf(stderr, "invalid native guest thread request: entry=%08X arg=%08X stack=%08X priority=%d processor=%d\n",
                r[1], r[2], r[3], (int32_t)r[0], (int32_t)r[4]);
        exit(21);
    }
    uint32_t handle = next_guest_thread_handle++;
    if (handle == 0x50000u) put32(0x0060F138u, 0xFFFFFFFFu);
    static unsigned thread_trace_count;
    if (getenv("DQ8_TRACE_THREADS") && thread_trace_count++ < 32u)
        fprintf(stderr, "guest CreateThread entry=%08X arg=%08X argwords=%08X/%08X/%08X/%08X stack=%08X priority=%d handle=%08X pc=%08X\n",
                r[1], r[2], r[2] < GUEST_RAM_SIZE - 16u ? mem32(r[2]) : 0u,
                r[2] < GUEST_RAM_SIZE - 16u ? mem32(r[2] + 4u) : 0u,
                r[2] < GUEST_RAM_SIZE - 16u ? mem32(r[2] + 8u) : 0u,
                r[2] < GUEST_RAM_SIZE - 16u ? mem32(r[2] + 12u) : 0u,
                r[3], (int32_t)r[0], handle, current_guest_pc);
    /* Keep the default deterministic while allowing an opt-in cooperative
       startup pass. Guest waits/sleeps yield the child back to its creator. */
    if (!guest_threads_enabled()) {
        r[0] = 0u;
        r[1] = handle;
        return 0u;
    }
    if (guest_thread_count >= 64u) { fputs("native guest thread table exhausted\n", stderr); exit(21); }
    GuestThread *thread = &guest_threads[guest_thread_count++];
    memset(thread, 0, sizeof(*thread));
    thread->r[0] = r[2];
    thread->r[13] = r[3] & ~7u;
    thread->r[14] = 0xFFFFFFFFu;
    thread->pc = r[1];
    thread->guest_thread_id = handle;
    thread->priority = (int32_t)r[0];
    thread->tls_base = 0x1FF82000u + guest_thread_count * 0x200u;
    memset(ram + thread->tls_base, 0, 0x200u);
    thread->active = 1;
    /* Register the new thread's stack extent and check it does not collide
       with a still-live peer (no-overlap invariant, t_eb3aef76).  The stack
       top is the high bound; the low bound starts at the top page and grows
       down as the thread runs (see guest_stack_track call sites). */
    {
        unsigned slot = guest_thread_count - 1u;
        if (slot < GUEST_THREAD_STACK_SLOTS) {
            uint32_t top = r[3] & ~7u;
            guest_stack_env_init();
            /* Layout fix: the guest allocator sometimes hands the same block
               to two live threads (e.g. tops 0x0868C710 / 0x0868C6D8, 56 B
               apart) or hands a stack out OVER a live heap record (the
               round-9 0x08690358..88 buffer), which aliases frames and lets
               another context stomp the saved lr (the 0x005CB6E0 escapes,
               round-11 NULL-call).  Relocate any stack that sits in an
               allocator region, or collides with a live peer, into a private
               disjoint slot unless explicitly disabled. */
            int heap_stack = guest_stack_in_allocator_region(top);
            if (!guest_stack_no_relocate &&
                (heap_stack || guest_stack_collides(top, GUEST_STACK_BLOCK))) {
                uint32_t relocated = guest_reloc_next + GUEST_STACK_BLOCK;
                if (relocated <= GUEST_RELOC_BASE + GUEST_RELOC_SLOTS * GUEST_STACK_BLOCK) {
                    guest_reloc_next = relocated;
                    fprintf(stderr,
                            "guest thread-stack RELOCATE slot %u: 0x%08X -> 0x%08X "
                            "(%s; layout fix t_eb3aef76)\n",
                            slot, top, relocated,
                            heap_stack ? "top inside guest allocator region"
                                       : "collides with a live peer");
                    top = relocated;
                    ++guest_stack_relocated;
                    r[3] = top;
                    thread->r[13] = top;
                }
            }
            guest_stack_top[slot] = top;
            guest_stack_low[slot] = top & ~0xFFFu;
            guest_stack_live[slot] = 1;
            if (guest_stack_track(slot, top)) ++guest_stack_overlaps;
        }
    }
    memcpy(thread->fpr, fpr, sizeof(fpr));
    thread->fpscr = fpscr;
    thread->exclusive_addr = 0xFFFFFFFFu;
    thread->exclusive_size = 4u;
    /* Creating a runnable kernel thread does not make its creator yield
       synchronously.  Let the main guest finish the current startup critical
       section; the scheduler dispatches the worker at the next real wait. */
    r[0] = 0u;
    r[1] = handle;
    return 0u;
}
static void schedule_guest_thread(void) {
    PF_ENTER(PF_AUDIO);
    dsp_audio_tick();
    PF_EXIT();
    if (!guest_threads_enabled() || thread_execution_depth || !guest_thread_count) return;
    movie_trace_tick();
    uint64_t now = dq8_guest_now_ns();
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        GuestThread *candidate = &guest_threads[i];
        if (!candidate->active || !candidate->blocked) continue;
        /* Readiness pass.  Producers wake single-object waiters themselves
           through guest_notify_handle(), so this pass only covers objects
           whose readiness can change without one: multi-object waits, and
           thread objects which become ready exactly once, on exit.  Polling
           events here as well would let the GSP vblank relay wake itself,
           because it parks right after signalling its own event in order to
           pace the frame.  An expired wait never completes here either
           (audit R2-05): the deadline pass below retires it with its own
           timeout instead of letting readiness hand it a foreign signal. */
        int expired = candidate->wake_ns && now >= candidate->wake_ns;
        movie_trace_wake_reason = "ready";
        if (candidate->wait_n_active && !expired) {
            uint32_t ready_index = 0xFFFFFFFFu;
            int result = guest_poll_wait_n(candidate->wait_handles, candidate->wait_count,
                                           candidate->wait_all, &ready_index, 1,
                                           candidate->guest_thread_id);
            if (result > 0) guest_wait_finish(candidate, 0u, ready_index, !candidate->wait_all);
            else if (result < 0) guest_wait_finish(candidate, 0xD8E007F7u, 0xFFFFFFFFu, 0);
        } else if (!expired && candidate->wait_handle >= 0x50000u && candidate->wait_handle < 0x50040u &&
                   guest_object_ready(candidate->wait_handle, candidate->guest_thread_id)) {
            guest_wait_finish(candidate, 0u, 0u, 0);
        }
        /* Explicit deadline: a wait no producer satisfied records its own
           timeout here.  Readiness was checked first, so a signal arriving in
           the same tick still wins.  wake_result 0 means "keep the recorded
           result", which is what sleeps and the GSP vblank pacing park want;
           multi-object waits and joins still time out at their deadline. */
        if (candidate->active && candidate->blocked && candidate->wake_ns &&
            now >= candidate->wake_ns) {
            uint32_t result = candidate->wake_result;
            movie_trace_wake_reason = "timeout";
            if (!result && (candidate->wait_n_active ||
                            (candidate->wait_handle >= 0x50000u && candidate->wait_handle < 0x50040u)))
                result = 0x09401BFEu;
            if (result)
                guest_wait_finish(candidate, result, 0xFFFFFFFFu, candidate->wait_n_active);
            else
                guest_wait_release(candidate);
        }
    }
    unsigned index = guest_thread_cursor++ % guest_thread_count;
    for (unsigned checked = 0; checked < guest_thread_count; ++checked) {
        GuestThread *thread = &guest_threads[index];
        if (thread->active && !thread->blocked) {
            uint32_t *parent_regs = active_guest_regs;
            uint32_t parent_pc = current_guest_pc, parent_previous_pc = previous_guest_pc;
            uint32_t parent_fpr[32]; memcpy(parent_fpr, fpr, sizeof(fpr));
            uint32_t parent_fpscr = fpscr;
            uint32_t parent_thread_id = current_guest_thread_id;
            uint32_t parent_tls_base = active_tls_base;
            memcpy(fpr, thread->fpr, sizeof(fpr)); fpscr = thread->fpscr;
            /* Single-core exclusive monitor (audit R2-02): a guest context
               switch clears the reservation, the way the 3DS kernel clears
               exclusive state on every switch.  Neither the incoming
               thread's stale LDREX state nor the outgoing one survives. */
            exclusive_addr = 0xFFFFFFFFu; exclusive_size = 4u;
            current_guest_thread_id = thread->guest_thread_id;
            active_tls_base = thread->tls_base;
            thread_yield_requested = 0;
            thread_stopped_by_yield = 0;
            thread_blocked_requested = 0;
            thread_wait_handle_requested = 0;
            thread_wait_address_requested = 0;
            thread_wait_arbiter_requested = 0;
            thread_wait_type_requested = 0;
            thread_wait_value_requested = 0;
            thread_sleep_requested_ns = 0;
            thread_wake_result_requested = 0;
            static unsigned schedule_trace_count;
            if (getenv("DQ8_TRACE_THREADS") && schedule_trace_count++ < 128u)
                fprintf(stderr, "guest scheduler resume thread=%u pc=%08X arg=%08X\n", index, thread->pc, thread->r[0]);
            ++thread_execution_depth;
            translated_from(thread->r, thread->pc, thread);
            --thread_execution_depth;
            /* SP low-water tracking (t_eb3aef76): record how far down this
               thread's stack actually reached so the no-overlap invariant in
               guest_stack_track sees the true extent, not just the top page. */
            if (index < GUEST_THREAD_STACK_SLOTS)
                guest_stack_track(index, thread->r[13]);
            if (thread_blocked_requested) {
                ++perf_worker_parks;
                thread->blocked = 1;
                thread->wait_handle = thread_wait_handle_requested;
                thread->wait_address = thread_wait_address_requested;
                thread->wait_arbiter = thread_wait_arbiter_requested;
                thread->wait_type = thread_wait_type_requested;
                thread->wait_value = thread_wait_value_requested;
                thread->wake_ns = thread_sleep_requested_ns ? dq8_guest_now_ns() + thread_sleep_requested_ns : 0u;
                thread->wake_result = thread_wake_result_requested;
                if (movie_trace()) {
                    thread->block_since_ns = dq8_guest_now_ns();
                    ++thread->blocked_count;
                    ++movie_trace_parks;
                    movie_trace_event("park thread=%08X pc=%08X handle=%08X addr=%08X "
                                      "type=%u value=%08X sleep_ns=%llu wake_result=%08X",
                                      thread->guest_thread_id, thread->pc,
                                      thread_wait_handle_requested,
                                      thread_wait_address_requested,
                                      thread_wait_type_requested,
                                      thread_wait_value_requested,
                                      (unsigned long long)thread_sleep_requested_ns,
                                      thread_wake_result_requested);
                }
            }
            thread_blocked_requested = 0;
            thread_wait_handle_requested = 0;
            thread_wait_address_requested = 0;
            thread_wait_arbiter_requested = 0;
            thread_wait_type_requested = 0;
            thread_wait_value_requested = 0;
            thread_sleep_requested_ns = 0;
            thread_wake_result_requested = 0;
            if (getenv("DQ8_TRACE_THREADS") && schedule_trace_count < 128u)
                fprintf(stderr, "guest scheduler yield thread=%u pc=%08X active=%d r0=%08X\n", index, thread->pc, thread->active, thread->r[0]);
            memcpy(thread->fpr, fpr, sizeof(fpr)); thread->fpscr = fpscr;
            thread->exclusive_addr = 0xFFFFFFFFu; thread->exclusive_size = 4u;
            current_guest_thread_id = parent_thread_id;
            active_tls_base = parent_tls_base;
            memcpy(fpr, parent_fpr, sizeof(fpr)); fpscr = parent_fpscr;
            exclusive_addr = 0xFFFFFFFFu; exclusive_size = 4u;
            active_guest_regs = parent_regs;
            current_guest_pc = parent_pc; previous_guest_pc = parent_previous_pc;
            return;
        }
        index = (index + 1u) % guest_thread_count;
    }
}
static uint32_t svc_get_process_id(uint32_t output, uint32_t process) {
    if (process != 0xFFFF8001u && process != 0xFFFF8000u && process != 0x40000u) {
        fprintf(stderr, "unimplemented 3DS SVC GetProcessId: process=%08X\n", process);
        exit(19);
    }
    put32(output, 1u);
    return 0;
}
static uint32_t svc_get_thread_id(uint32_t *r) {
    static unsigned trace_count;
    if (getenv("DQ8_TRACE_THREAD_ID") && trace_count++ < 128u)
        fprintf(stderr, "guest GetThreadId pc=%08X current=%08X input=%08X\n",
                current_guest_pc, current_guest_thread_id, r[1]);
    if (r[1] != 0 && r[1] != 0xFFFF8000u && r[1] != 0xFFFF8001u)
        return 0xD8E007F7u;
    /* The guest uses thread IDs to distinguish recursive mutex ownership.
       All cooperative workers must not inherit the main guest's identity. */
    r[1] = current_guest_thread_id;
    if (getenv("DQ8_TRACE_THREAD_ID") && trace_count <= 128u)
        fprintf(stderr, "guest GetThreadId result=%08X\n", r[1]);
    return 0;
}
static uint32_t svc_get_resource_limit(uint32_t *r) {
    if (r[1] != 0xFFFF8001u && r[1] != 0xFFFF8000u && r[1] != 1u)
        return 0xD8E007F7u;
    /* The game's wrapper stores the returned handle from r1 to its output. */
    r[1] = 0x30000u;
    return 0;
}
static uint32_t svc_get_resource_values(uint32_t *r, int limits) {
    if (r[1] != 0x30000u) return 0xD8E007F7u;
    for (uint32_t i = 0; i < r[3] && i < 64u; ++i) {
        uint32_t type = mem32(r[2] + i * 4u);
        uint64_t value = 0;
        if (limits && type == 1u) value = 0x04000000u; /* RESLIMIT_COMMIT = APPMEMALLOC */
        else if (limits && type == 2u) value = 0x30u;  /* process thread limit */
        else if (limits && type >= 3u && type <= 8u) value = 0x80u;
        else if (!limits && type == 2u) value = 1u;   /* main process thread */
        put32(r[0] + i * 8u, (uint32_t)value);
        put32(r[0] + i * 8u + 4u, (uint32_t)(value >> 32));
    }
    return 0;
}
/* ----------------------------------------------------------------------
 * Wait-object state machine (audit A07).
 *
 * Every waitable object has one readiness predicate and one atomic
 * acquire/consume step:
 *
 *   mutex   ready while free, or already held by this waiter (recursive);
 *           acquiring takes ownership and bumps the nesting count.
 *   event   ready while signaled; a one-shot event is consumed by the
 *           acquire, a manual-reset event keeps its signal.
 *   thread  ready once the target thread has exited; acquiring it changes
 *           no object state.
 *
 * Producers never touch `blocked`: they make an object ready and call
 * guest_notify_handle(), which selects waiters by priority and lets each of
 * them acquire in turn.  The object's own post-acquire state bounds that
 * loop, so one-shot events and mutexes hand off to exactly one waiter while
 * manual-reset events and thread objects wake every waiter.  A waiter is
 * turned runnable only after its SVC result has been recorded.
 * ---------------------------------------------------------------------- */
static int guest_object_ready(uint32_t handle, uint32_t thread_id) {
    if (handle >= 0x10000u && handle < next_mutex_handle) {
        unsigned slot = handle - 0x10000u;
        if (!mutex_live[slot]) return 0;
        if (!mutex_locked[slot]) return 1;
        /* The holder may take it again: kernel mutexes are recursive. */
        return mutex_owner[slot] != 0 && mutex_owner[slot] == thread_id;
    }
    if (handle < next_event_handle && event_live[handle]) return event_signaled[handle] != 0;
    if (handle >= 0x50000u && handle < 0x50040u) {
        for (unsigned i = 0; i < guest_thread_count; ++i)
            if (guest_threads[i].guest_thread_id == handle) return !guest_threads[i].active;
    }
    return 0; /* SRV's dormant notification object and unknown handles stay not-ready */
}
static int guest_object_acquire(uint32_t handle, uint32_t thread_id) {
    if (handle >= 0x10000u && handle < next_mutex_handle) {
        unsigned slot = handle - 0x10000u;
        if (!mutex_live[slot]) return 0;
        if (mutex_locked[slot]) {
            if (mutex_owner[slot] == 0 || mutex_owner[slot] != thread_id) return 0;
        } else {
            mutex_owner[slot] = thread_id;
        }
        mutex_locked[slot] = 1;
        if (mutex_recursion[slot] < 255u) ++mutex_recursion[slot];
        return 1;
    }
    if (handle < next_event_handle && event_live[handle]) {
        if (!event_signaled[handle]) return 0;
        if (!event_manual_reset[handle]) event_signaled[handle] = 0;
        return 1;
    }
    if (handle >= 0x50000u && handle < 0x50040u) {
        for (unsigned i = 0; i < guest_thread_count; ++i)
            if (guest_threads[i].guest_thread_id == handle) return !guest_threads[i].active;
    }
    return 0;
}
/* Is parked thread `t` waiting on `handle`, directly or inside WaitN? */
static int guest_thread_waits_on(const GuestThread *t, uint32_t handle) {
    if (!t->active || !t->blocked) return 0;
    if (t->wait_n_active) {
        for (unsigned i = 0; i < t->wait_count; ++i)
            if (t->wait_handles[i] == handle) return 1;
        return 0;
    }
    /* Sleeps and arbiter waits carry no kernel handle. */
    return t->wait_handle != 0 && t->wait_handle == handle;
}
/* Pure probe: would this waiter complete right now? No state is changed. */
static int guest_wait_ready(const GuestThread *t, uint32_t handle) {
    if (t->wait_n_active) {
        uint32_t index = 0xFFFFFFFFu;
        return guest_poll_wait_n(t->wait_handles, t->wait_count, t->wait_all,
                                 &index, 0, t->guest_thread_id) != 0;
    }
    return guest_object_ready(handle, t->guest_thread_id);
}
/* Hand the thread back to the scheduler without touching its result. */
static void guest_wait_release(GuestThread *t) {
    if (movie_trace()) {
        uint64_t now = host_monotonic_ns();
        const char *reason = movie_trace_wake_reason ? movie_trace_wake_reason : "none";
        if (t->block_since_ns) {
            t->blocked_ns += now - t->block_since_ns;
            t->block_since_ns = 0;
        }
        if (movie_trace_wake_reason && !strcmp(movie_trace_wake_reason, "signal"))
            ++movie_trace_wake_signal;
        else if (movie_trace_wake_reason && !strcmp(movie_trace_wake_reason, "timeout"))
            ++movie_trace_wake_timeout;
        else if (movie_trace_wake_reason && !strcmp(movie_trace_wake_reason, "ready"))
            ++movie_trace_wake_ready;
        else ++movie_trace_wake_other;
        movie_trace_event("wake thread=%08X reason=%s result=%08X pc=%08X",
                          t->guest_thread_id, reason, t->r[0], t->pc);
    }
    t->wait_handle = 0;
    t->wait_address = 0;
    t->wait_type = 0;
    t->wait_value = 0;
    t->wait_arbiter = 0;
    t->wait_count = 0;
    t->wait_all = 0;
    t->wait_n_active = 0;
    t->blocked = 0;
    t->wake_ns = 0;
    t->wake_result = 0;
}
/* Record the SVC result of a parked wait, then make the thread runnable.
   The index is written only for wait-any; the kernel leaves wait-all's
   output alone. */
static void guest_wait_finish(GuestThread *t, uint32_t result, uint32_t index, int has_index) {
    static unsigned trace_count;
    uint32_t handle = t->wait_handle;
    int was_n = t->wait_n_active;
    if (has_index) t->r[1] = index;
    t->r[0] = result;
    if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait) && trace_count++ < 256u)
        fprintf(stderr, "guest wait complete thread=%08X handle=%08X waitn=%d result=%08X index=%08X\n",
                t->guest_thread_id, handle, was_n, result, has_index ? index : 0xFFFFFFFFu);
    guest_wait_release(t);
}
/* Make `handle` available, one waiter at a time: select (lowest priority
   number wins, ties go to the earliest registered thread), acquire/consume,
   record the result, run.  Returns how many waiters the object satisfied. */
static unsigned guest_notify_handle(uint32_t handle) {
    unsigned woken = 0;
    uint64_t now = dq8_guest_now_ns();
    movie_trace_wake_reason = "signal";
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < guest_thread_count; ++i) {
            GuestThread *t = &guest_threads[i];
            if (!guest_thread_waits_on(t, handle)) continue;
            /* Audit R2-05: a waiter whose deadline has already passed is
               retired by the scheduler's deadline pass, never by a
               producer's signal: it must not consume an event that belongs
               to an active waiter.  (A zero-time poll never parks, so a
               wake_ns-free waiter is always eligible here.) */
            if (t->wake_ns && now >= t->wake_ns) continue;
            if (!guest_wait_ready(t, handle)) continue;
            if (best < 0 || t->priority < guest_threads[best].priority) best = (int)i;
        }
        if (best < 0) break;
        GuestThread *t = &guest_threads[best];
        if (t->wait_n_active) {
            uint32_t index = 0xFFFFFFFFu;
            int result = guest_poll_wait_n(t->wait_handles, t->wait_count, t->wait_all,
                                           &index, 1, t->guest_thread_id);
            /* A failed acquire after a successful probe cannot happen: no
               other context runs in between.  Recording an error still
               guarantees the loop terminates. */
            guest_wait_finish(t, result > 0 ? 0u : 0xD8E007F7u,
                              result > 0 ? index : 0xFFFFFFFFu, result > 0 && !t->wait_all);
        } else {
            int acquired = guest_object_acquire(handle, t->guest_thread_id);
            guest_wait_finish(t, acquired ? 0u : 0xD8E007F7u, 0u, 0);
        }
        ++woken;
    }
    return woken;
}
static uint32_t svc_create_mutex(uint32_t *r) {
    if (next_mutex_handle >= 0x10040u) { fputs("native guest mutex table exhausted\n", stderr); exit(16); }
    uint32_t handle = next_mutex_handle++;
    unsigned slot = handle - 0x10000u;
    mutex_live[slot] = 1;
    mutex_locked[slot] = r[1] != 0;
    mutex_owner[slot] = r[1] != 0 ? current_guest_thread_id : 0u;
    mutex_recursion[slot] = r[1] != 0 ? 1u : 0u;
    r[1] = handle;
    static unsigned trace_count;
    if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait) && trace_count++ < 128u)
        fprintf(stderr, "guest CreateMutex pc=%08X handle=%08X initial=%u\n",
                current_guest_pc, handle, mutex_locked[slot]);
    return 0;
}
static uint32_t svc_release_mutex(uint32_t handle) {
    if (handle >= 0x10000u && handle < next_mutex_handle && mutex_live[handle - 0x10000u]) {
        unsigned slot = handle - 0x10000u;
        static unsigned trace_count;
        if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait) && trace_count++ < 128u)
            fprintf(stderr, "guest ReleaseMutex pc=%08X thread=%08X handle=%08X locked=%u owner=%08X recursion=%u\n",
                    current_guest_pc, current_guest_thread_id, handle,
                    mutex_locked[slot], mutex_owner[slot], mutex_recursion[slot]);
        if (!mutex_locked[slot]) return 0; /* nothing is held: no waiter can be served */
        if (mutex_owner[slot] && mutex_owner[slot] != current_guest_thread_id) {
            /* Audit R2-04: a foreign release must not break mutual
               exclusion.  The 3DS kernel returns WrongLockingThread and
               leaves the owner, the nesting depth and the waiters exactly
               as they were (0xD8E0041F: desc=31, kernel module, same
               summary/level as the other kernel argument errors). */
            if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait))
                fprintf(stderr, "guest ReleaseMutex DENIED thread=%08X handle=%08X owner=%08X recursion=%u\n",
                        current_guest_thread_id, handle, mutex_owner[slot], mutex_recursion[slot]);
            return 0xD8E0041Fu;
        }
        if (mutex_recursion[slot] > 1u) { --mutex_recursion[slot]; return 0; } /* still held */
        mutex_recursion[slot] = 0;
        mutex_locked[slot] = 0;
        mutex_owner[slot] = 0;
        guest_notify_handle(handle); /* hand-off: the winner re-locks with itself as owner */
        return 0;
    }
    fprintf(stderr, "unimplemented 3DS SVC ReleaseMutex: handle=%08X\n", handle);
    exit(17);
}
static uint32_t svc_create_event(uint32_t *r) {
    if (next_event_handle >= 256u) { fputs("native guest event table exhausted\n", stderr); exit(22); }
    uint32_t handle = next_event_handle++;
    event_live[handle] = 1;
    event_manual_reset[handle] = r[1] != 0;
    event_signaled[handle] = 0;
    if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait))
        fprintf(stderr, "guest CreateEvent pc=%08X thread=%08X handle=%08X manual=%u\n",
                current_guest_pc, current_guest_thread_id, handle, event_manual_reset[handle]);
    /* SVC veneers store the returned handle from r1 through their saved
       output pointer after the kernel call. */
    r[1] = handle;
    return 0;
}
static uint32_t svc_signal_event(uint32_t handle) {
    if (handle == 0u || handle == 0xFFFFFFFFu) return 0;
    if (handle < next_event_handle && event_live[handle]) {
        if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait))
            fprintf(stderr, "guest SignalEvent pc=%08X thread=%08X handle=%08X\n",
                    current_guest_pc, current_guest_thread_id, handle);
        event_signaled[handle] = 1;
        /* One-shot events are consumed by the acquiring waiter, so exactly
           one of them resumes; manual-reset events stay signaled and wake
           everybody still parked on the handle. */
        guest_notify_handle(handle);
        return 0;
    }
    fprintf(stderr, "unimplemented 3DS SVC SignalEvent: handle=%08X\n", handle); exit(23);
}
static uint32_t svc_clear_event(uint32_t handle) {
    static unsigned trace_count;
    if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait) && trace_count++ < 128u)
        fprintf(stderr, "guest ClearEvent pc=%08X thread=%08X handle=%08X live=%u\n",
                current_guest_pc, current_guest_thread_id, handle,
                handle < next_event_handle ? event_live[handle] : 0u);
    if (handle == 0u || handle == 0xFFFFFFFFu || handle == 0x30001u || handle == 0x30002u) return 0;
    if (handle < next_event_handle && event_live[handle]) {
        event_signaled[handle] = 0;
        if (handle == dsp_pipe_interrupt_event) dsp_source_event_pending = 0;
        return 0;
    }
    fprintf(stderr, "unimplemented 3DS SVC ClearEvent: handle=%08X\n", handle); exit(24);
}
static uint32_t svc_wait_synchronization_inner(uint32_t handle, uint64_t timeout) {
    static unsigned trace_count;
    if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait) && trace_count++ < 128u)
        fprintf(stderr, "guest WaitSynchronization pc=%08X thread=%08X handle=%08X timeout=%016llX live=%u signaled=%u mutex_locked=%u\n",
                current_guest_pc, current_guest_thread_id, handle,
                (unsigned long long)timeout,
                handle < next_event_handle ? event_live[handle] : 0u,
                handle < next_event_handle ? event_signaled[handle] : 0u,
                handle >= 0x10000u && handle < next_mutex_handle
                    ? mutex_locked[handle - 0x10000u] : 0u);
    if (handle == 0u) {
        /* Zero is not a kernel waitable handle. Returning the guest's invalid
           handle result lets the caller take its normal no-event path. */
        return 0xD8E007F7u;
    }
    if (handle >= 0x50000u && handle < 0x50040u) {
        int found = 0;
        for (unsigned i = 0; i < guest_thread_count; ++i)
            if (guest_threads[i].guest_thread_id == handle) { found = 1; break; }
        /* A main-thread join must drive the scheduler-backed wait below;
           returning success here would let it free a live worker's stack. */
        if (!found) return 0xD8E007F7u;
    }
    if (handle == 2u) {
        /* SRV's process-notification event is dormant during this boot path;
           park its waiter and let the main guest continue initialization. */
        if (thread_execution_depth) {
            thread_yield_requested = 1;
            thread_blocked_requested = 1;
            thread_wait_handle_requested = handle;
        }
        return 0;
    }
    /* Feed the guest's real GSP interrupt worker through its shared-memory
       relay queue. PDC0 is the top-screen vblank interrupt.  Audit R2-06:
       vblank pacing is internal, so it must never override the caller's
       own wait semantics.  A zero-time poll returns immediately, a finite
       wait never sleeps past its own deadline, and the provisional
       success belongs only to the pacing park that really reached the
       frame deadline. */
    if (gsp_relay_registered && handle == gsp_relay_event &&
        handle < next_event_handle && event_live[handle] && !event_signaled[handle]) {
        uint32_t q = gsp_shared_base + gsp_thread_id * 0x40u;
        uint8_t cur = mem8(q), count = mem8(q + 1u);
        static unsigned relay_trace_count;
        if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait) && relay_trace_count++ < 8u)
            fprintf(stderr, "GSP relay wait event=%u queue=%08X cur=%u count=%u live=%u registered=%u timeout=%016llX\n",
                    handle, q, cur, count, event_live[handle], gsp_relay_registered,
                    (unsigned long long)timeout);
        (void)cur;
        (void)count;
        uint64_t now = dq8_guest_now_ns();
        if (!gsp_next_vblank_ns || now >= gsp_next_vblank_ns || gsp_vbl_owed_pending()) {
            /* The frame deadline is due: deliver every elapsed vblank on the
               wall-clock chain (PACE, t_3463e625: identical for both render
               modes; the enqueue signals the relay event, so the wait
               completes through the ordinary readiness path below). */
            guest_vblank_service(now);
            host_display_pump_events();
            host_display_present();
        } else if (timeout && thread_execution_depth) {
            /* Vblank is still in the future: park the relay thread until
               it is due, capped by the caller's own timeout.  The pacing
               park keeps its provisional success (wake_result 0); when
               the caller's timeout would land first, the deadline pass
               records the real kernel timeout instead.  A zero timeout
               skips this branch entirely and polls below.
               The provisional PDC0/PDC1 enqueue that this park used to
               perform is REMOVED for every render mode (it was removed for
               DQ8_SPLIT_RENDER=1 in F5 after the movie scene measured 92/s
               from that double delivery): it delivered a vblank AHEAD of the
               frame deadline, so every wait returned after ~0 ms instead of
               the remaining span, and the guest's frame clock became the
               guest's own loop rate rather than 60 Hz.  The wall-clock
               service fires the vblank when the park expires, and the wait
               re-entry completes it. */
            uint64_t until_vblank = gsp_next_vblank_ns - now;
            uint64_t span = (uint64_t)timeout < until_vblank ? (uint64_t)timeout : until_vblank;
            int timeout_lands_first = (int64_t)timeout > 0 && (uint64_t)timeout < until_vblank;
            thread_yield_requested = 1;
            thread_blocked_requested = 1;
            thread_wait_handle_requested = handle;
            thread_sleep_requested_ns = idle_stretch_wait(span);
            thread_wake_result_requested = (split_render_enabled() || timeout_lands_first)
                ? 0x09401BFEu : 0u;
            return 0;
        }
    }
    /* Readiness first, timeout second: like svcWaitSynchronization, an
       object that is already ready is acquired even for a zero-length wait. */
    if (guest_object_ready(handle, current_guest_thread_id)) {
        if (!guest_object_acquire(handle, current_guest_thread_id)) return 0xD8E007F7u;
        return 0;
    }
    if (!timeout) return 0x09401BFEu; /* polled, and nothing was ready */
    if (thread_execution_depth) {
        /* Park.  The producer that makes this object ready completes the
           wait through guest_notify_handle(), the deadline records the
           timeout; either way the result is written before this thread runs
           again, so a parked wait never resumes on its provisional value. */
        thread_yield_requested = 1;
        thread_blocked_requested = 1;
        thread_wait_handle_requested = handle;
        thread_sleep_requested_ns = (int64_t)timeout < 0 ? 0 : idle_stretch_wait(timeout);
        thread_wake_result_requested = (int64_t)timeout < 0 ? 0 : 0x09401BFEu;
        return 0;
    }
    /* The main guest context cannot park, so service workers, vblanks, HID,
       DSP, and the host window run inside this call until the handle signals
       or the wait expires. */
    if (thread_execution_depth == 0) {
        uint64_t start_ns = dq8_guest_now_ns();
        uint64_t deadline_ns = (int64_t)timeout < 0 ? 0u : start_ns + idle_stretch_wait(timeout);
        for (unsigned spin = 0; spin < 20000000u; ++spin) {
            ++perf_main_wait_spins;
            if (dq8_env_on("DQ8_TRACE_WAIT", &dq8_env_trace_wait) && (spin % 2000u) == 0u)
                fprintf(stderr, "main-wait spin=%u handle=%08X depth=%u\n", spin, handle, thread_execution_depth);
            guest_service_host_deadlines(dq8_guest_now_ns());
            schedule_guest_thread();
            uint64_t now = dq8_guest_now_ns();
            guest_service_host_deadlines(now);
            if (guest_object_ready(handle, current_guest_thread_id)) {
                if (!guest_object_acquire(handle, current_guest_thread_id)) return 0xD8E007F7u;
                return 0;
            }
            if (deadline_ns && now >= deadline_ns)
                return 0x09401BFEu;
            if (guest_any_runnable_thread()) continue;
            if (!runnable_nap()) { guest_pacing_nap(deadline_ns, 2000000ull); continue; }
            guest_wait_until_deadline(deadline_ns);
        }
    }
    return 0x09401BFEu; /* timeout */
}
/* TEMPO wrapper: counts one completed wait per logic iteration of whatever
   loop owns `handle` and the wall gap between consecutive completions. */
static uint32_t svc_wait_synchronization(uint32_t handle, uint64_t timeout) {
    int tempo_slot = tempo_wait_enter(handle);
    uint32_t result = svc_wait_synchronization_inner(handle, timeout);
    if (result == 0u) tempo_wait_ready(tempo_slot);
    return result;
}
/* Probe (and optionally acquire) until the complete wait condition is met.
   Returns -1 for invalid handles, 0 for pending, 1 for acquired.  With
   consume=0 this is a pure readiness probe and changes no object state. */
static int guest_poll_wait_n(const uint32_t *handles, uint32_t count, uint32_t all,
                             uint32_t *index, int consume, uint32_t thread_id) {
    unsigned available[64], ready_count = 0;
    uint32_t first = 0xFFFFFFFFu;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t h = handles[i]; int ready = 0, valid = 0;
        if (h < next_event_handle && event_live[h]) {
            valid = 1;
        } else if (h >= 0x10000u && h < next_mutex_handle && mutex_live[h - 0x10000u]) {
            valid = 1;
        } else if (h >= 0x50000u && h < 0x50040u) {
            for (unsigned t = 0; t < guest_thread_count; ++t)
                if (guest_threads[t].guest_thread_id == h) { valid = 1; break; }
        } else if (h == 2u) valid = 1; /* SRV notification semaphore: empty */
        if (!valid) return -1;
        ready = guest_object_ready(h, thread_id);
        available[i] = ready;
        if (ready) { ++ready_count; if (first == 0xFFFFFFFFu) first = i; }
    }
    if (all ? ready_count != count : !ready_count) return 0;
    if (!consume) { if (!all) *index = first; return 1; }
    for (unsigned i = 0; i < count; ++i) {
        if (!available[i] || (!all && i != first)) continue;
        if (!guest_object_acquire(handles[i], thread_id)) return -1;
    }
    if (!all) *index = first;
    return 1;
}
static uint32_t svc_wait_synchronization_n(uint32_t *r) {
    /* The real veneer at 0x0011D3E8 saves the output pointer, then loads
       timeout low/high into r0/r4. Only the returned index lives in r1. */
    uint32_t handles_va = r[1], count = r[2], all = r[3];
    uint64_t timeout = (uint64_t)r[0] | ((uint64_t)r[4] << 32);
    if (count > 64u || handles_va >= GUEST_RAM_SIZE || count > (GUEST_RAM_SIZE - handles_va) / 4u)
        return 0xD8E007F7u;
    uint32_t handles[64];
    for (unsigned i = 0; i < count; ++i) handles[i] = mem32(handles_va + i * 4u);
    int tempo_slot = count ? tempo_wait_enter(handles[0]) : -1;
    uint32_t index = r[1];
    int result = guest_poll_wait_n(handles, count, all, &index, 1, current_guest_thread_id);
    if (result < 0) return 0xD8E007F7u;
    if (result) { r[1] = index; tempo_wait_ready(tempo_slot); return 0; }
    if (!timeout) return 0x09401BFEu;
    if (thread_execution_depth) {
        for (unsigned i = 0; i < guest_thread_count; ++i)
            if (guest_threads[i].guest_thread_id == current_guest_thread_id) {
                GuestThread *thread = &guest_threads[i];
                memcpy(thread->wait_handles, handles, count * sizeof(uint32_t));
                thread->wait_count = count;
                thread->wait_n_active = 1;
                thread->wait_all = all;
                thread_yield_requested = thread_blocked_requested = 1;
                thread_wait_handle_requested = 0;
                thread_sleep_requested_ns = (int64_t)timeout < 0 ? 0 : idle_stretch_wait(timeout);
                thread_wake_result_requested = (int64_t)timeout < 0 ? 0 : 0x09401BFEu;
                r[1] = 0xFFFFFFFFu;
                return 0;
            }
        return 0xD8E007F7u;
    }
    uint64_t start = dq8_guest_now_ns();
    uint64_t deadline = (int64_t)timeout >= 0 ? start + idle_stretch_wait(timeout) : 0u;
    for (;;) {
        schedule_guest_thread();
        uint64_t now = dq8_guest_now_ns();
        guest_service_host_deadlines(now);
        index = 0xFFFFFFFFu;
        result = guest_poll_wait_n(handles, count, all, &index, 1, current_guest_thread_id);
        if (result < 0) return 0xD8E007F7u;
        if (result) { r[1] = index; tempo_wait_ready(tempo_slot); return 0; }
        if (deadline && now >= deadline) {
            r[1] = 0xFFFFFFFFu; return 0x09401BFEu;
        }
        if (guest_any_runnable_thread()) continue;
        if (!runnable_nap()) { guest_pacing_nap(deadline, 1000000ull); continue; }
        guest_wait_until_deadline(deadline);
    }
}
static uint32_t svc_unmap_memory_block(uint32_t handle, uint32_t address) {
    fprintf(stderr, "unimplemented 3DS SVC UnmapMemoryBlock: handle=%08X addr=%08X\n", handle, address);
    exit(26);
}
static uint32_t svc_map_memory_block(uint32_t handle, uint32_t address) {
    if (handle >= 0x30000u && handle < 0x30100u && address < GUEST_RAM_SIZE) {
        if (handle == 0x30010u) {
            if(address > GUEST_RAM_SIZE-0x1000u)return 0xD8E007F7u;
            native_hid_base=address;memset(ram+address,0,0x1000u);
            native_hid_index=0;native_hid_next_update=0;
            trace_hid_read_base=address;trace_hid_read_end=address+0x1000u;
        }
        if (handle == 0x30020u) gsp_shared_base = address;
        if (dq8_env_on("DQ8_TRACE_GPU", &dq8_env_trace_gpu))
            fprintf(stderr, "MapMemoryBlock handle=%08X addr=%08X\n", handle, address);
        return 0;
    }
    fprintf(stderr, "unimplemented 3DS SVC MapMemoryBlock: handle=%08X addr=%08X\n", handle, address);
    exit(25);
}
static int guest_memory_cstr(uint32_t va, char *out, unsigned cap) {
    uint32_t mapped;
    unsigned n = 0;
    if (!guest_memory_range(va, 1u, &mapped)) { if (cap) out[0] = 0; return 0; }
    while (n + 1u < cap && mapped + n < GUEST_RAM_SIZE) {
        uint8_t ch = ram[mapped + n];
        out[n++] = (char)ch;
        if (!ch) return (int)n - 1;
    }
    if (cap) out[cap - 1u] = 0;
    return (int)n;
}
/* The guest's format helper at 0x143974 (Thumb vsnprintf-style) is mistranslated
   or incomplete: it returns a character count but leaves the destination empty.
   Serve the simple %s / %d / %x patterns the title path uses with the host
   snprintf so resource paths like "anim/foo.bflan" are built correctly. */
static int guest_host_snprintf(uint32_t *r) {
    uint32_t dest_va = r[0], size = r[1], maxlen = r[2], fmt_va = r[3];
    uint32_t dest;
    unsigned cap = size;
    if (maxlen && maxlen + 1u < cap) cap = maxlen + 1u;
    if (!cap) return 0;
    if (!guest_memory_range(dest_va, cap, &dest)) return -1;
    char fmt[256];
    if (guest_memory_cstr(fmt_va, fmt, sizeof(fmt)) <= 0) return -1;
    char out[512];
    unsigned oi = 0;
    unsigned arg_i = 0;
    for (const char *f = fmt; *f && oi + 1u < sizeof(out);) {
        if (*f != '%') { out[oi++] = *f++; continue; }
        ++f;
        const char *spec_start = f;
        while (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0') ++f;
        while (*f >= '0' && *f <= '9') ++f;
        if (*f == '.') { ++f; while (*f >= '0' && *f <= '9') ++f; }
        const char *spec_end = f; /* flags + width + precision, verbatim */
        if (*f == 'l') { ++f; if (*f == 'l') ++f; }
        char conv = *f ? *f++ : 's';
        /* Rebuild the original spec (e.g. "%02d") so flags/width/precision are
           honoured instead of dropped; the dialog's "TextBox_%02d_%02d" child
           names depend on the zero padding. The length modifier is deliberately
           not carried over: guest long is 32-bit and every argument is fetched
           as one guest word, so host int/unsigned match exactly. */
        char spec[32];
        unsigned sp = 0;
        spec[sp++] = '%';
        for (const char *p = spec_start; p != spec_end && sp + 2u < sizeof(spec); ++p)
            spec[sp++] = *p;
        spec[sp++] = conv;
        spec[sp] = 0;
        uint32_t arg_va = mem32(r[13] + arg_i * 4u);
        arg_i++;
        char piece[256];
        int n = 0;
        if (conv == 's') {
            char str[256];
            n = guest_memory_cstr(arg_va, str, sizeof(str));
            if (n < 0) n = 0;
            n = snprintf(piece, sizeof(piece), spec, str);
        } else if (conv == 'c') {
            n = snprintf(piece, sizeof(piece), spec, (int)(unsigned char)arg_va);
        } else if (conv == 'd' || conv == 'i') {
            n = snprintf(piece, sizeof(piece), spec, (int32_t)arg_va);
        } else if (conv == 'u') {
            n = snprintf(piece, sizeof(piece), spec, arg_va);
        } else if (conv == 'x' || conv == 'X') {
            n = snprintf(piece, sizeof(piece), spec, arg_va);
        } else if (conv == 'p') {
            n = snprintf(piece, sizeof(piece), "%08X", arg_va);
        } else if (conv == '%') {
            piece[0] = '%';
            n = 1;
            arg_i--;
        } else {
            piece[0] = '%';
            piece[1] = conv;
            n = 2;
        }
        if (n < 0) n = 0;
        if (n >= (int)sizeof(piece)) n = (int)sizeof(piece) - 1; /* wide specs may ask for more than piece holds */
        if (n > 0) {
            if (oi + (unsigned)n >= sizeof(out)) n = (int)(sizeof(out) - 1u - oi);
            memcpy(out + oi, piece, (unsigned)n);
            oi += (unsigned)n;
        }
    }
    out[oi] = 0;
    if (oi >= cap) oi = cap - 1u;
    memcpy(ram + dest, out, oi + 1u);
    return (int)oi;
}
/* t_d1be62fc: diagnosis hook — snapshot guest RAM at wall-clock offsets so the
   in-game play-time counter (u32 seconds) can be measured without the save
   menu.  DQ8_DUMP_RAM_AT="30,90" DQ8_DUMP_RAM_DIR=/path (off by default; the
   dump is a plain 1 GiB image of guest RAM at that offset). */
static void dq8_dump_ram_service(void) {
    static uint64_t t0;
    static int init, all_done;
    static unsigned idx;
    static const char *spec;
    static const char *cur;
    if (!init) {
        init = 1;
        spec = getenv("DQ8_DUMP_RAM_AT");
        if (spec && *spec) t0 = host_monotonic_ns();
        else all_done = 1;
    }
    if (all_done) return;
    {
        double elapsed = (double)(host_monotonic_ns() - t0) / 1e9;
        if (!cur) cur = spec;
        while (*cur && idx < 31u) {
            char *end = NULL;
            double at = strtod(cur, &end);
            if (end == cur) { all_done = 1; return; }
            if (elapsed < at) return;   /* earliest pending offset not reached */
            {
                const char *dir = getenv("DQ8_DUMP_RAM_DIR");
                char path[512];
                FILE *f;
                snprintf(path, sizeof path, "%s/ram_%u.bin",
                         (dir && *dir) ? dir : ".", idx);
                f = fopen(path, "wb");
                if (f) { fwrite(ram, 1, GUEST_RAM_SIZE, f); fclose(f); }
                fprintf(stderr, "DUMPRAM at=%.0fs elapsed=%.2fs wrote=%s\n", at, elapsed, path);
            }
            ++idx;
            if (*end != ',') { all_done = 1; return; }
            cur = end + 1;
            if (!*cur) { all_done = 1; return; }
        }
    }
}
/* DQ8_SKIP_MOVIE: skip the opening movie.  Read once from the environment;
   0/unset keeps the unmodified movie behaviour, anything else arms the bypass
   in the dispatch loop (see the DQ8_SKIP_MOVIE comment in translated_from). */
static int dq8_skip_movie = -1;
static unsigned dq8_skip_movie_bypasses;
static int dq8_skip_movie_enabled(void) {
    if (dq8_skip_movie < 0) {
        const char *value = getenv("DQ8_SKIP_MOVIE");
        dq8_skip_movie = (value && *value && *value != '0') ? 1 : 0;
        if (dq8_skip_movie)
            fprintf(stderr, "DQ8_SKIP_MOVIE=1: intro movie will be bypassed at startup\n");
    }
    return dq8_skip_movie;
}
/* Drive the game's own movie-end state for the player object passed in r[0]
   and return from the play-and-wait entry 0x002FA318 the way it returns when
   the movie is over.  The natural end of the opening movie is: the last clip
   finishes (finish check 0x002FA81C stores 3 at obj+0xC), the controller stops
   the player and stop-and-wait 0x002FA0F0 finds the stream ring empty and
   stores 7 = "ended" (0x002FA11C), so the next play-wait call - entered with
   state 7, i.e. neither 1 nor 8 - returns 0 and the clip loop finishes instead
   of restarting.  We store 7 directly, replicate play-wait's release of the
   player lock at obj+0x20 (0x0014C914, which wakes the player/driver worker;
   without it the worker stays parked and the player is never parked after the
   stop), and return 0.  The driver only ever observes the ended state, so no
   video frame is decoded, converted or presented. */
static void dq8_skip_movie_bypass(uint32_t *r, uint32_t *pc, int *running) {
    uint32_t object = r[0], lock = object + 0x20u;
    put32(object + 0xCu, 7u);
    put32(lock, 1u);
    for (unsigned i = 0; i < guest_thread_count; ++i) {
        GuestThread *thread = &guest_threads[i];
        if (thread->active && thread->blocked &&
            thread->wait_address == lock && thread->wait_type == 1u)
            guest_wait_finish(thread, 0u, 0u, 0);
    }
    ++dq8_skip_movie_bypasses;
    if (dq8_skip_movie_bypasses <= 8u)
        fprintf(stderr, "DQ8_SKIP_MOVIE: bypass movie -> menu state (obj=%08X bypass=%u)\n",
                object, dq8_skip_movie_bypasses);
    r[0] = 0u;
    if (r[14] == 0xFFFFFFFFu) *running = 0;
    else *pc = r[14];
}
/* Shared dispatch-boundary predicate (finding A05).
   The split batch dispatcher (tools/split_dispatch.py) runs up to
   CHAIN_BATCH guest instructions inside one part call, so any instruction the
   main loop must guard with a semantic hook - the guest snprintf shim, the
   page-list fast-path shortcut, a PC-gated trace hook, or an explicitly
   traced PC from DQ8_TRACE_PC - has to end its batch and return to
   dq8_main.c, which re-runs the boundary and re-dispatches.  Over-approximating
   is safe (the boundary re-checks every condition before acting); omitting a
   pc silently bypasses the hook in split builds only, which is exactly the
   A05 single/split divergence.  tools/test_split_hooks.py fails when a pc
   compared by the boundary or by guest_fast_page_list is missing here. */
static int dq8_pc_needs_boundary(uint32_t pc) {
    /* Fast prefilter: a page that contains no boundary pc (nearly all of
       them) returns here after two hot L1 loads instead of walking the exact
       switch below.  dq8_pc_boundary_page is generated from the same pc list
       as the switch, so a 0 can never skip a static hook; runtime
       DQ8_TRACE_PC pcs are not known at build time, so their (short) list is
       only skipped while it is empty.  A 1 falls through to the exact
       checks. */
    if (!dq8_pc_boundary_page[(pc >> 12) & 0xFFFu] && trace_guest_pc_count == 0)
        return 0;
    switch (pc) {
/* BOUNDARY_CASES */
        return 1;
    default:
        break;
    }
    for (unsigned i = 0; i < trace_guest_pc_count; ++i)
        if (pc == trace_guest_pcs[i]) return 1;
    return 0;
}
/* SAVESTATE_IMPL (t_d50c9503): spliced from src/native_savestate.c */
/* S1_BLOCK_DEF */
/* S1_CFG_DEF */
static void translated_from(uint32_t *r, uint32_t start_pc, GuestThread *context) {
    active_guest_regs = r;
    /* PERF-02: attribute the dispatch itself, so a session that spends its
       wall time inside worker contexts is visible even when the main counter
       is flat. */
    if (context) { ++perf_worker_dispatches; ++context->dispatch_calls; }
    else ++perf_main_dispatches;
    uint32_t pc = context ? context->pc : start_pc; int running = 1; uint64_t steps=0;
    uint64_t next_schedule_step = 100000ull, next_pump_step = 1000000ull;
    uint64_t next_progress_step = 100000000ull;
    int n=context ? context->n : 0, z=context ? context->z : 0;
    int c=context ? context->c : 0, v=context ? context->v : 0;
    int ge=context ? context->ge : 0;
    /* SAVESTATE (t_d50c9503): a startup `--load-state` restores the main
       context's condition flags here (regs/pc come in through r/start_pc). */
    if (!context) savestate_take_resume(&n, &z, &c, &v, &ge);
    (void)n; (void)z; (void)c; (void)v; (void)ge;
    uint64_t dq8_gt_last = 0;
    while (running && steps++ < (thread_execution_depth ? 5000ull : bench_step_limit)) {
        /* DETERMINISTIC (t_115d297c): fold retired main-context instructions
           into the virtual guest clock at a coarse watermark.  Off by default
           (one predictable branch per dispatch iteration). */
        if (dq8_det_mode && !context && steps >= dq8_det_next_main_mark) {
            dq8_det_next_main_mark = steps + 4096ull;
            dq8_det_fold_main(steps);
        }
        if (dq8_guest_time_enabled && thread_execution_depth == 0) {
            uint64_t dq8_gt_now = dq8_guest_cpu_ns();
            if (dq8_gt_last) dq8_guest_time_ns += dq8_gt_now - dq8_gt_last;
            dq8_gt_last = dq8_gt_now;
        }
        /* SPIKETRAP (t_5a3c517a): the guest PC that owns the current frame's
           host work, for the SPIKETRAP line.  One load+branch per dispatch. */
        if (pf_trap_on > 0) pf_trap_pc = pc;
        /* SAVESTATE_HOOK (t_d50c9503): F5/F9 are serviced only here, at a main-
           context dispatch boundary where every guest thread's state is already
           quiescent (pc = next instruction, regs/flags = its input) and no
           worker slice is on the host stack.  On a load the restored pc/regs/
           flags replace the live ones and the loop continues in the state's
           scene without a reboot. */
        if (!context && thread_execution_depth == 0 &&
            savestate_service(&pc, r, &n, &z, &c, &v, &ge)) {
            previous_guest_pc = pc;
            if (getenv("DQ8_TRACE_SAVESTATE"))
                fprintf(stderr, "SAVESTATE: dispatch resumed at pc=%08X\n", pc);
        }
        if (!context && thread_execution_depth == 0) dq8_dump_ram_service();
        if (guest_fast_page_list(r, &pc, &n, &z, &c, &v, &steps)) continue;
        if ((pc & ~1u) == 0x00143974u) {
            r[0] = (uint32_t)guest_host_snprintf(r);
            if (r[14] == 0xFFFFFFFFu) running = 0;
            else pc = r[14];
            continue;
        }
        /* DQ8_SKIP_MOVIE=1: bypass the opening movie.  The guest initiates the
           intro through the play-and-wait entry 0x002FA318 (startup path
           0x0024F8CC, player object in r0, state word obj+0xC: 1/8 = idle).
           Instead of playing we drive the game's own movie-end transition and
           return "finished", so the caller continues through exactly the code
           it runs after a natural movie end and no video frame is decoded,
           converted or presented.  With the flag unset the hook is never
           entered and the intro plays exactly as before. */
        if (pc == 0x002FA318u) {
            if (perf_movie_entries == 0u && (getenv("DQ8_TRACE_Y2R") || movie_trace()))
                fprintf(stderr, "MOVIE_ENTRY t_ns=%llu\n",
                        (unsigned long long)host_monotonic_ns());
            ++perf_movie_entries;   /* scene marker: intro player reached */
            movie_trace_event("movie_entry n=%llu obj=%08X state=%u",
                              (unsigned long long)perf_movie_entries, r[0],
                              r[0] < GUEST_RAM_SIZE ? mem32(r[0] + 0xCu) : 0xFFFFFFFFu);
        }
        if (pc == 0x002FA318u && dq8_skip_movie_enabled()) {
            uint32_t movie_object = r[0];
            if (movie_object < GUEST_RAM_SIZE - 0x30u) {
                uint32_t movie_state = mem32(movie_object + 0xCu);
                if (movie_state == 1u || movie_state == 8u) {
                    dq8_skip_movie_bypass(r, &pc, &running);
                    continue;
                }
            }
        }
        /* The native main guest must not monopolize execution in a polling
           loop. The 3DS kernel preempts runnable threads even when guest code
           makes no SVC call; approximate that with periodic round-robin slices.
           The cadence uses a watermark instead of `steps % N == 0` because
           split parts advance `steps` by a whole batch between main-loop
           iterations: an exact-multiple test silently skips every mark a
           batch steps over.  While `steps` only ever grows by 1 (single-TU
           dispatch, or fast-page-list jumps that overshoot), the watermark
           fires on exactly the same steps as the old modulo test - first at
           N, then at N*2, ... - so single-TU behaviour is unchanged; split
           batches fire at most CHAIN_BATCH instructions late and never skip. */
        if (!context && steps >= next_schedule_step) {
            next_schedule_step = steps + 100000ull;
            if (guest_threads_enabled()) schedule_guest_thread();
        }
        /* Keep the host window responsive during long guest spins that never
           reach the GSP-relay vblank wait. */
        if (!context && steps >= next_pump_step) {
            next_pump_step = steps + 1000000ull;
            host_display_pump_events();
            host_display_present();
        }
        if (!context) main_guest_pc = pc;
        previous_guest_pc = current_guest_pc;
        current_guest_pc = pc;
        if ((pc == 0x004725E8u || pc == 0x004725ECu) && getenv("DQ8_TRACE_UI_OBJECT")) {
            static unsigned ui_object_trace_hits;
            if (ui_object_trace_hits++ < 16u) {
                fprintf(stderr, "UI object trace pc=%08X r0=%08X r4=%08X r5=%08X lr=%08X sp=%08X fields=",
                        pc, r[0], r[4], r[5], r[14], r[13]);
                if (r[0] < GUEST_RAM_SIZE - 0x50u) {
                    for (unsigned off = 0; off <= 0x50u; off += 4u)
                        fprintf(stderr, "%08X%s", mem32(r[0] + off), off == 0x50u ? "\n" : "/");
                } else {
                    fputs("<outside guest RAM>\n", stderr);
                }
            }
        }
        if ((pc == 0x0011A1C0u || pc == 0x0011A1ECu) && getenv("DQ8_TRACE_LOOKUP")) {
            static unsigned lookup_trace_hits;
            if (lookup_trace_hits++ < 64u) {
                uint32_t container = r[0], key = r[1];
                uint32_t base = 0, lo = 0, hi = 0;
                if (pc == 0x0011A1C0u && container && container < GUEST_RAM_SIZE - 0x40u)
                    base = mem32(container + 4u);
                if (base && base < GUEST_RAM_SIZE - 0x38u) {
                    lo = mem32(base + 0x28u);
                    hi = mem32(base + 0x2Cu);
                }
                fprintf(stderr, "lookup pc=%08X container=%08X key=%08X table=%08X lo=%08X hi=%08X r0=%08X lr=%08X\n",
                        pc, container, key, base, lo, hi, r[0], r[14]);
                if (lookup_trace_hits == 1u && 0x0054B644u < GUEST_RAM_SIZE - 0x20u) {
                    fprintf(stderr, "manager 54B644:");
                    for (unsigned off = 0; off < 0x20u; off += 4u)
                        fprintf(stderr, " +%02X=%08X", off, mem32(0x0054B644u + off));
                    fputc('\n', stderr);
                }
            }
        }
        if (pc == 0x004725ECu && getenv("DQ8_SCAN_UI_HEAP")) {
            static unsigned ui_heap_scanned;
            if (!ui_heap_scanned++) {
                unsigned matches = 0;
                const uint32_t starts[] = { 0x08000000u, 0x14000000u };
                const uint32_t ends[] = { 0x09300000u, 0x16D00000u };
                for (unsigned region = 0; region < 2u && matches < 32u; ++region) {
                    for (uint32_t a = starts[region]; a < ends[region] - 4u && matches < 32u; ++a) {
                        if (memcmp(ram + a, "FLYT", 4u) == 0) {
                            fprintf(stderr, "guest FLYT signature region=%u addr=%08X bytes=", region, a);
                            for (unsigned i = 0; i < 48u; ++i) fprintf(stderr, "%02X", ram[a + i]);
                            fputc('\n', stderr);
                            ++matches;
                        }
                    }
                }
                fprintf(stderr, "guest heap FLYT signatures found=%u\n", matches);
            }
        }
        if ((pc == 0x003AAD54u || pc == 0x003AB3E8u) && getenv("DQ8_TRACE_BFLYT")) {
            static unsigned bflyt_trace_hits;
            if (bflyt_trace_hits++ < 32u) {
                uint32_t object = pc == 0x003AAD54u ? r[0] : r[10];
                uint32_t data = pc == 0x003AAD54u ? r[2] : mem32(object + 0x24u);
                fprintf(stderr, "BFLYT parser trace pc=%08X object=%08X data=%08X regs=%08X/%08X/%08X/%08X bytes=",
                        pc, object, data, r[0], r[1], r[2], r[3]);
                for (unsigned i = 0; i < 64u && data + i < GUEST_RAM_SIZE; ++i)
                    fprintf(stderr, "%02X", mem8(data + i));
                fprintf(stderr, " data-fields=");
                if (data < GUEST_RAM_SIZE - 0x40u) {
                    for (unsigned off = 0; off <= 0x40u; off += 4u)
                        fprintf(stderr, "%08X%s", mem32(data + off), off == 0x40u ? " " : "/");
                } else {
                    fputs("<outside guest RAM> ", stderr);
                }
                fprintf(stderr, " fields=");
                if (object < GUEST_RAM_SIZE - 0x50u) {
                    for (unsigned off = 0; off <= 0x50u; off += 4u)
                        fprintf(stderr, "%08X%s", mem32(object + off), off == 0x50u ? "\n" : "/");
                } else {
                    fputs("<outside guest RAM>\n", stderr);
                }
            }
        }
        if (pc == 0x0016572Cu && getenv("DQ8_TRACE_OBJECT")) {
            uint32_t o = r[0], data = mem32(o + 0x8Cu);
            fprintf(stderr, "resource object pc=%08X args=%08X/%08X/%08X/%08X sp=%08X fields=",
                    pc, r[0], r[1], r[2], r[3], r[13]);
            for (unsigned off = 0; off <= 0xA4u; off += 4u) fprintf(stderr, "%08X%s", mem32(o + off), off == 0xA4u ? " " : "/");
            fprintf(stderr, "data@%08X=", data);
            for (unsigned i = 0; i < 48u && data + i < GUEST_RAM_SIZE; ++i) fprintf(stderr, "%02X", mem8(data + i));
            fprintf(stderr, " arg1@%08X=", r[1]);
            for (unsigned i = 0; i < 48u && r[1] + i < GUEST_RAM_SIZE; ++i) fprintf(stderr, "%02X", mem8(r[1] + i));
            fputc('\n', stderr);
        }
        if ((pc == 0x00223E40u || pc == 0x00223E44u || pc == 0x00223E48u) && getenv("DQ8_TRACE_SELECTION")) {
            uint32_t node = r[4] + 0x20u;
            fprintf(stderr, "selection pc=%08X object=%08X node=%08X node+0..20=", pc, r[4], node);
            for (unsigned off = 0; off <= 0x20u; off += 4u)
                fprintf(stderr, "%08X%s", mem32(node + off), off == 0x20u ? "" : "/");
            fprintf(stderr, " lookup=%08X descriptor=%08X\n", r[1], mem32(r[5] + 8u));
        }
        if (pc == 0x0011D608u && getenv("DQ8_TRACE_LOOKUP")) {
            static unsigned snprintf_hits;
            if (snprintf_hits++ < 16u) {
                uint32_t name_ptr = mem32(r[13]);
                fprintf(stderr, "bflan-snprintf dest=%08X fmt=%08X name_ptr=%08X s=",
                        r[0], r[3], name_ptr);
                for (unsigned i = 0; i < 32u && name_ptr + i < GUEST_RAM_SIZE; ++i) {
                    uint8_t ch = mem8(name_ptr + i);
                    if (!ch) break;
                    fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                }
                fputc('\n', stderr);
            }
        }
        if (pc == 0x0011D62Cu && getenv("DQ8_TRACE_LOOKUP")) {
            static unsigned keyret_hits;
            if (keyret_hits++ < 16u)
                fprintf(stderr, "key-after-sarc r0=%08X r1=%08X lr=%08X\n", r[0], r[1], r[14]);
        }
        if (pc == 0x003A5028u && getenv("DQ8_TRACE_SARC_LOOKUP")) {
            static unsigned ent_hits;
            if (ent_hits++ < 12u)
                fprintf(stderr, "SARC enter r0=%08X r1=%08X r2=%08X r10set s2=",
                        r[0], r[1], r[2]);
            if (ent_hits <= 12u) {
                for (unsigned i = 0; i < 24u && r[2] + i < GUEST_RAM_SIZE; ++i) {
                    uint8_t ch = mem8(r[2] + i);
                    if (!ch) break;
                    fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                }
                fputc('\n', stderr);
            }
        }
        if ((pc == 0x003A5134u || pc == 0x003A5138u || pc == 0x003A50C4u) && getenv("DQ8_TRACE_SARC_LOOKUP")) {
            static unsigned path_hits;
            if (path_hits++ < 24u) {
                fprintf(stderr, "path pc=%08X r0=%08X r1=%08X r10=%08X s10=", pc, r[0], r[1], r[10]);
                for (unsigned i = 0; i < 32u && r[10] + i < GUEST_RAM_SIZE; ++i) {
                    uint8_t ch = mem8(r[10] + i);
                    if (!ch) break;
                    fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                }
                fprintf(stderr, " dest16=");
                uint32_t d = (pc == 0x003A50C4u) ? r[1] : r[13] + 0x10u;
                for (unsigned i = 0; i < 24u; ++i)
                    fprintf(stderr, "%02X", d + i < GUEST_RAM_SIZE ? mem8(d + i) : 0);
                fprintf(stderr, " s=");
                for (unsigned i = 0; i < 32u && d + i < GUEST_RAM_SIZE; ++i) {
                    uint8_t ch = mem8(d + i);
                    if (!ch) break;
                    fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                }
                fputc('\n', stderr);
            }
        }
        if (pc == 0x003A3718u && getenv("DQ8_TRACE_SARC_LOOKUP")) {
            static unsigned binsearch_hits;
            if (binsearch_hits++ < 24u)
                fprintf(stderr, "SARC binsearch computed=%08X table=%08X mid=%u count=%u tbl=%08X\n",
                        r[6], r[3], r[5], r[7], r[1]);
        }
        if (pc == 0x003A5088u && getenv("DQ8_TRACE_SARC_LOOKUP")) {
            static unsigned sarc_ext_hits;
            if (sarc_ext_hits++ < 32u)
                fprintf(stderr, "SARC extcheck r4=%08X b24=%02X b25=%02X key_r1=%08X lr=%08X\n",
                        r[4], mem8(r[4] + 0x24u), mem8(r[4] + 0x25u), r[1], r[14]);
        }
        if ((pc == 0x003A5110u || pc == 0x003A5114u || pc == 0x003A5118u || pc == 0x003A511Cu) && getenv("DQ8_TRACE_SARC_LOOKUP")) {
            static unsigned sarc_ret_hits;
            if (sarc_ret_hits++ < 32u)
                fprintf(stderr, "SARC lookup result pc=%08X r0=%08X r4=%08X r10=%08X lr=%08X\n",
                        pc, r[0], r[4], r[10], r[14]);
        }
        if (pc == 0x003A36B8u && getenv("DQ8_TRACE_SARC_LOOKUP")) {
            static unsigned hash_hits;
            if (hash_hits++ < 16u) {
                fprintf(stderr, "SARC hash r0=%08X r1=%08X s=",
                        r[0], r[1]);
                uint32_t sptr = r[1];
                for (unsigned i = 0; i < 16u && sptr + i < GUEST_RAM_SIZE; ++i) {
                    uint8_t ch = mem8(sptr + i);
                    if (!ch) break;
                    fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                }
                if (r[0] && r[0] < GUEST_RAM_SIZE - 0x18u)
                    fprintf(stderr, " tbl=%08X +00=%08X +04=%08X +08=%08X +0C=%08X\n",
                            r[0], mem32(r[0]), mem32(r[0] + 4u), mem32(r[0] + 8u), mem32(r[0] + 0xCu));
                else
                    fputc('\n', stderr);
            }
        }
        if (pc == 0x003A5028u && getenv("DQ8_TRACE_SARC_LOOKUP") &&
            r[0] < GUEST_RAM_SIZE - 0x40u && mem32(r[0]) == 0x0059C40Cu) {
            uint32_t head = mem32(r[0] + 8u), sentinel = r[0] + 8u;
            uint32_t archive_item = head;
            fprintf(stderr, "SARC lookup object=%08X key=%08X chars=%c%c%c%c head=%08X sentinel=%08X obj0-20=",
                    r[0], r[1], (char)(r[1] >> 24), (char)(r[1] >> 16),
                    (char)(r[1] >> 8), (char)r[1], head, sentinel);
            for (unsigned off = 0; off <= 0x20u; off += 4u)
                fprintf(stderr, "%08X%s", mem32(r[0] + off), off == 0x20u ? "\n" : "/");
            for (unsigned item = 0; item < 12u && head != sentinel &&
                 head < GUEST_RAM_SIZE - 0x50u; ++item) {
                fprintf(stderr, "  item[%u] addr=%08X words=", item, head);
                for (unsigned off = 0; off <= 0x40u; off += 4u)
                    fprintf(stderr, "%08X%s", mem32(head + off), off == 0x40u ? " text=" : "/");
                for (unsigned off = 0x20u; off < 0x48u; ++off) {
                    uint8_t ch = (uint8_t)mem8(head + off);
                    fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                }
                fputc('\n', stderr);
                head = mem32(head);
            }
            uint32_t archive_base = mem32(archive_item + 8u);
            uint32_t name_table = mem32(archive_item + 0x10u);
            uint32_t node_table = mem32(archive_item + 0x18u);
            uint32_t file_count = mem32(archive_item + 0x14u);
            uint32_t data_base = mem32(archive_item + 0x1Cu);
            static uint32_t sarc_dumped_archive;
            if (archive_base != sarc_dumped_archive && file_count <= 128u &&
                node_table < GUEST_RAM_SIZE - file_count * 0x10u && name_table < GUEST_RAM_SIZE) {
                sarc_dumped_archive = archive_base;
                fprintf(stderr, "SARC index archive=%08X names=%08X nodes=%08X data=%08X count=%u\n",
                        archive_base, name_table, node_table, data_base, file_count);
                if (archive_base < GUEST_RAM_SIZE - 0x30u) {
                    fprintf(stderr, "SARC hdr:");
                    for (unsigned off = 0; off < 0x30u; off += 4u)
                        fprintf(stderr, " +%02X=%08X", off, mem32(archive_base + off));
                    fputc('\n', stderr);
                }
                if (file_count <= 8u && node_table < GUEST_RAM_SIZE - file_count * 0x10u) {
                    for (uint32_t i = 0; i < file_count; ++i) {
                        fprintf(stderr, "  node[%u]", i);
                        for (unsigned off = 0; off < 0x10u; off += 4u)
                            fprintf(stderr, " %08X", mem32(node_table + i * 0x10u + off));
                        fputc('\n', stderr);
                    }
                }
                for (uint32_t i = 0; i < file_count; ++i) {
                    uint32_t entry = node_table + i * 0x10u;
                    if (entry > GUEST_RAM_SIZE - 0x10u) break;
                    uint32_t name_offset = (mem32(entry + 4u) & 0x00FFFFFFu) * 4u;
                    uint32_t name = name_table + name_offset;
                    fprintf(stderr, "  file[%02u] range=%08X..%08X name=", i,
                            mem32(entry + 8u), mem32(entry + 12u));
                    for (unsigned j = 0; j < 80u && name + j < GUEST_RAM_SIZE; ++j) {
                        uint8_t ch = (uint8_t)mem8(name + j);
                        if (!ch) break;
                        fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                    }
                    fputc('\n', stderr);
                }
            }
        }
        if (pc == 0x003A50C4u && getenv("DQ8_TRACE_STRING_OBJECT") &&
            r[1] < GUEST_RAM_SIZE - 0x40u) {
            uint32_t object = r[1];
            uint32_t data = mem32(object);
            uint32_t length = mem32(object + 4u);
            fprintf(stderr, "string lookup object=%08X words=%08X/%08X/%08X/%08X data=%08X len=%u bytes=",
                    object, mem32(object), mem32(object + 4u), mem32(object + 8u),
                    mem32(object + 12u), data, length);
            if (data < GUEST_RAM_SIZE) {
                for (unsigned i = 0; i < 24u && data + i < GUEST_RAM_SIZE; ++i) {
                    uint8_t ch = (uint8_t)mem8(data + i);
                    fprintf(stderr, "%02X", ch);
                }
            }
            fputc('\n', stderr);
        }
        if (dq8_env_on("DQ8_TRACE_FORMAT_PATH", &dq8_env_trace_fmtpath)) {
            if (pc == 0x00143974u && r[14] == 0x003A5138u) {
                trace_format_path_active = 1;
                trace_format_path_dest = r[0];
                trace_format_path_base = 0;
                trace_format_path_char_count = 0;
                fprintf(stderr, "path-format begin dest=%08X size=%08X fmt=%08X args=%08X/%08X/%08X\n",
                        r[0], r[1], r[3], mem32(r[13]), mem32(r[13] + 4u),
                        mem32(r[13] + 8u));
            }
            if (trace_format_path_active &&
                (pc == 0x002FD5F9u || pc == 0x002FD7D7u ||
                 pc == 0x002FD93Fu || pc == 0x002FD941u)) {
                fprintf(stderr, "path-format callback pc=%08X lr=%08X r0-r5=%08X/%08X/%08X/%08X/%08X/%08X obj=%08X/%08X/%08X/%08X/%08X/%08X desc=%08X/%08X/%08X\n",
                        pc, r[14], r[0], r[1], r[2], r[3], r[4], r[5],
                        mem32(r[4]), mem32(r[4] + 4u), mem32(r[4] + 8u),
                        mem32(r[4] + 12u), mem32(r[4] + 16u), mem32(r[4] + 32u),
                        mem32(r[1]), mem32(r[1] + 4u), mem32(r[1] + 8u));
            }
            if (trace_format_path_active && pc == 0x002FD5F9u &&
                r[14] == 0x002FD941u && r[1] < GUEST_RAM_SIZE - 8u) {
                uint32_t cursor = mem32(r[1]);
                if (!trace_format_path_base) trace_format_path_base = cursor;
                if (trace_format_path_char_count < 96u)
                    fprintf(stderr, "path-format char=%04X cursor=%08X\n",
                            r[0] & 0xFFFFu, cursor);
                ++trace_format_path_char_count;
            }
            if (trace_format_path_active && pc == 0x003A5138u) {
                uint32_t data = mem32(trace_format_path_dest);
                fprintf(stderr, "path-format end count=%u dest=%08X/%08X text=",
                        trace_format_path_char_count, data,
                        mem32(trace_format_path_dest + 4u));
                if (trace_format_path_base < GUEST_RAM_SIZE) {
                    for (unsigned i = 0; i < 48u; i += 2u) {
                        uint32_t ch = mem16(trace_format_path_base + i);
                        if (!ch) break;
                        fputc(ch >= 0x20u && ch < 0x7Fu ? (int)ch : '.', stderr);
                    }
                }
                fprintf(stderr, " object-bytes=");
                for (unsigned i = 0; i < 16u; ++i)
                    fprintf(stderr, "%02X", (uint8_t)mem8(trace_format_path_dest + i));
                fprintf(stderr, " data-bytes=");
                if (data < GUEST_RAM_SIZE - 32u) {
                    for (unsigned i = 0; i < 32u; ++i)
                        fprintf(stderr, "%02X", (uint8_t)mem8(data + i));
                }
                fputc('\n', stderr);
                trace_format_path_active = 0;
            }
        }
        if (pc == 0x003A50BCu && getenv("DQ8_TRACE_FORMAT_ARGS")) {
            fprintf(stderr, "format result len=%08X stack=%08X/%08X/%08X\n",
                    r[0], mem32(r[13]), mem32(r[13] + 4u), mem32(r[13] + 8u));
            for (unsigned arg = 0; arg < 3u; ++arg) {
                uint32_t ptr = mem32(r[13] + arg * 4u);
                fprintf(stderr, "  arg[%u]=%08X bytes=", arg, ptr);
                if (ptr < GUEST_RAM_SIZE - 32u) {
                    for (unsigned i = 0; i < 32u; ++i) {
                        uint8_t ch = (uint8_t)mem8(ptr + i);
                        if (!ch) break;
                        fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                    }
                }
                fputc('\n', stderr);
            }
            uint32_t output_object = r[13] + 0x10u;
            uint32_t output_data = mem32(output_object);
            fprintf(stderr, "  destination object=%08X/%08X scratch=%08X utf16=",
                    output_data, mem32(output_object + 4u), r[13] + 0x2Cu);
            for (unsigned i = 0; i < 24u; i += 2u) {
                uint32_t ch = mem16(r[13] + 0x2Cu + i);
                if (!ch) break;
                fputc(ch >= 0x20u && ch < 0x7Fu ? (int)ch : '.', stderr);
            }
            fputc('\n', stderr);
        }
        if (pc == 0x003A50C4u && getenv("DQ8_TRACE_STRING_OBJECT") &&
            r[13] < GUEST_RAM_SIZE - 0x80u) {
            fprintf(stderr, "  lookup frame utf16@%08X=", r[13] + 0x2Cu);
            for (unsigned i = 0; i < 48u; i += 2u) {
                uint32_t ch = mem16(r[13] + 0x2Cu + i);
                if (!ch) break;
                fputc(ch >= 0x20u && ch < 0x7Fu ? (int)ch : '.', stderr);
            }
            fputc('\n', stderr);
        }
        if (pc == 0x002FD5F9u && r[14] == 0x002FD941u &&
            getenv("DQ8_TRACE_FORMAT_SINK") && r[1] < GUEST_RAM_SIZE - 16u) {
            uint32_t cursor = mem32(r[1]);
            fprintf(stderr, "format sink char=%04X descriptor=%08X cursor=%08X end=%08X before=",
                    r[0] & 0xFFFFu, r[1], cursor, mem32(r[1] + 4u));
            for (unsigned i = 0; i < 8u && cursor + i * 2u < GUEST_RAM_SIZE; ++i) {
                uint32_t ch = mem16(cursor + i * 2u);
                if (!ch) break;
                fputc(ch >= 0x20u && ch < 0x7Fu ? (int)ch : '.', stderr);
            }
            fputc('\n', stderr);
        }
        unsigned trace_match = 0;
        for (unsigned trace_i = 0; trace_i < trace_guest_pc_count; ++trace_i)
            trace_match |= pc == trace_guest_pcs[trace_i];
        if (trace_match && (trace_guest_pc_hits++ < trace_guest_pc_max || trace_guest_pc_hits % 1000000u == 0u)) {
            fprintf(stderr, "guest trace pc=%08X r0-r12=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X mem[r0]=%08X lr=%08X sp=%08X stack:",
                    pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12],
                    r[0] < GUEST_RAM_SIZE ? mem32(r[0]) : 0xFFFFFFFFu, r[14], r[13]);
            for (unsigned trace_i = 0; trace_i < 12u; ++trace_i)
                fprintf(stderr, " +%02X=%08X", trace_i * 4u, mem32(r[13] + trace_i * 4u));
            fputc('\n', stderr);
        }
        if (steps >= next_progress_step) {
            next_progress_step = steps + 100000000ull;
            fprintf(stderr, "guest progress: steps=%llu pc=%08X\n", (unsigned long long)steps, pc);
        }
        /* S1_DISPATCH_HOOK */
        /* S1_CFG_HOOK */
        switch (pc >> 12) {
"#,
    );
    c = c.replace("/* PICA_GL_RUNTIME */", include_str!("pica_gl.c"));
    c = c.replace("/* FRAME_PROF_RUNTIME */", include_str!("frame_prof.c"));
    c = c.replace("/* PICA_VS_RUNTIME */", include_str!("pica_vs.c"));
    c = c.replace("/* NATIVE_FS_RUNTIME */", include_str!("native_fs.c"));
    c = c.replace("/* NATIVE_CECD_RUNTIME */", include_str!("native_cecd.c"));
    c = c.replace("/* NG_WORD_LIST_DATA */", include_str!("bad_word_list.h"));
    c = c.replace("/* NATIVE_HID_RUNTIME */", include_str!("native_hid.c"));
    c = c.replace("/* NATIVE_SPSC_RUNTIME */", include_str!("native_spsc.h"));
    c = c.replace("/* NATIVE_AUDIO_RUNTIME */", include_str!("native_audio.c"));
    c = c.replace("/* NATIVE_PRESENT_RUNTIME */", include_str!("native_present_worker.c"));
    c = c.replace("/* SAVESTATE_IMPL (t_d50c9503): spliced from src/native_savestate.c */",
                  include_str!("native_savestate.c"));
    c = c.replace("START_ADDR", &format!("0x{start:08X}"));
    // A05: single source of truth for the boundary-pc set.  The exact switch
    // in dq8_pc_needs_boundary (its cases) and the compile-time page
    // prefilter table (dq8_pc_boundary_page) are both generated from this
    // list so they cannot drift apart; tools/test_split_hooks.py additionally
    // checks the generated C against every pc the dispatch boundary and
    // guest_fast_page_list actually compare, and that the prefilter marks
    // exactly the pages those pcs live on.
    const BOUNDARY_PCS: &[u32] = &[
        // guest_fast_page_list shortcut and the guest_host_snprintf shim
        // (0x00143974, matched as pc & ~1 so both encodings land here), then
        // the remaining PC-gated boundary/trace hooks.
        0x0011_9198, 0x0014_3974, 0x0014_3975, 0x0011_A1C0, 0x0011_A1EC,
        0x003A_AD54, 0x003A_B3E8, 0x0016_572C, 0x0022_3E40, 0x0022_3E44,
        0x0022_3E48, 0x0011_D608, 0x0011_D62C, 0x003A_5028, 0x003A_5088,
        0x003A_50BC, 0x003A_50C4, 0x003A_5134, 0x003A_5138, 0x003A_3718,
        0x003A_36B8, 0x003A_5110, 0x003A_5114, 0x003A_5118, 0x003A_511C,
        0x002F_D5F9, 0x002F_D7D7, 0x002F_D93F, 0x002F_D941, 0x0047_25E8,
        0x0047_25EC,
        // DQ8_SKIP_MOVIE: movie play-and-wait entry (intro bypass hook)
        0x002F_A318,
    ];
    // S1b: an emitted-block build adds its entry to the A05 boundary set so
    // split-build batches end there and every arrival re-enters the main
    // loop where the block hook lives.  Without --s1-block the list (and the
    // generated C) is unchanged.
    let mut boundary_pcs: Vec<u32> = BOUNDARY_PCS.to_vec();
    if let Some(s1) = &s1 {
        boundary_pcs.push(s1.entry);
    }
    // S1c: same for the region CFG — split-build batches must end at the
    // region entry so every arrival re-enters the main loop where the CFG
    // hook lives (the region's own loop back-edge is compiled as a goto and
    // never re-dispatches).
    if let Some(cfg) = &s1_cfg {
        for entry in &cfg.entries {
            boundary_pcs.push(*entry);
        }
    }
    let boundary_cases: String = boundary_pcs
        .iter()
        .map(|pc| format!("    case 0x{pc:08X}u:\n"))
        .collect();
    let boundary_pages: String = boundary_pcs
        .iter()
        .map(|pc| format!("        [0x{:03X}u] = 1,\n", pc >> 12))
        .collect();
    c = c.replace("/* BOUNDARY_CASES */", &boundary_cases);
    c = c.replace("/* BOUNDARY_PAGES */", &boundary_pages);
    // t_911eff5c (adventure-log list corruption): the save-menu container
    // stores its list iterator with a 2-bit tag added (the setter at guest
    // 0x003F08C4 does `str r0, [r4,#8]` after `add r0, r1, #3`, i.e. ptr+3)
    // and the consumer chain (0x001E3F2C -> 0x002FDE1C -> 0x00300C74 ->
    // 0x0011FC48) forwards that value unchanged into the list helpers.
    // fn 0x00141D40 recovers the node base from the tag via the back-span
    // fixup `node - (u16(node+2) >> 8)`, which only works while byte node+6
    // still holds 3 (written at construction, pc 0x003A3EC0); the link/len
    // stores at 0x00141E44 / 0x00142134 overwrite it with (size >> 16) = 0,
    // so a tagged iterator keeps its +3: the helper then strh/str/strd's the
    // node header at (node+3), straddling the real node's +8/+0xC link words
    // and corrupting prev with partial pointer bytes (fault 0x0014218C ->
    // address 0xD0085BFD). Stripping the low 2 bits at the two
    // iterator-consuming entries is a no-op for the untagged iterators used
    // everywhere else (2201 observed calls) and restores a node-aligned base
    // for tagged ones.
    if let Some(stmt) = emitted.get_mut(&0x0014_1D40u32) {
        *stmt = format!("r[1] &= ~3u; {stmt}");
    }
    if let Some(stmt) = emitted.get_mut(&0x0014_1FC4u32) {
        *stmt = format!("r[1] &= ~3u; r[2] &= ~3u; {stmt}");
    }
    let mut pages = BTreeMap::<u32, Vec<(u32, &String)>>::new();
    for (pc, stmt) in &emitted {
        pages.entry(pc >> 12).or_default().push((*pc, stmt));
    }
    for (page, cases) in pages {
        c.push_str(&format!("case 0x{page:05X}: switch (pc) {{\n"));
        for (pc, stmt) in cases {
            c.push_str(&format!("case 0x{pc:08X}: {{ {stmt} break; }}\n"));
        }
        c.push_str("default: fprintf(stderr, \"translated PC escaped to %08X after %08X r0-r7=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X lr=%08X sp=%08X\\n\", pc, previous_guest_pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[14], r[13]); exit(3); } break;\n");
    }
    c.push_str("default: fprintf(stderr, \"translated PC escaped to %08X after %08X r0-r7=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X lr=%08X sp=%08X\\n\", pc, previous_guest_pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[14], r[13]); exit(3); } } if(!context && bench_enabled && steps >= bench_step_limit) bench_report(steps, pc); if(steps>=1000000000000ull) { fprintf(stderr, \"guest instruction budget exhausted at pc=%08X r0=%08X r1=%08X r13=%08X\\n\", pc, r[0], r[1], r[13]); exit(4); } if(context) { context->pc=pc; context->n=n; context->z=z; context->c=c; context->v=v; context->ge=ge; if(!running && !thread_stopped_by_yield) context->active=0; if(!running && !thread_stopped_by_yield && context >= &guest_threads[0] && context < &guest_threads[GUEST_THREAD_STACK_SLOTS]) guest_stack_release((unsigned)(context - guest_threads)); } if(context) { uint64_t retired = running ? (steps ? steps - 1u : 0u) : steps; context->retired_steps += retired; perf_worker_retired += retired; dq8_det_fold_worker(retired); } else { uint64_t retired = running ? (steps ? steps - 1u : 0u) : steps; perf_main_retired += retired; } }\n");
    c.push_str(&format!(r#"int main(int argc, char **argv) {{
    (void)mem8; (void)mem16; (void)put8; (void)put16; (void)arm_shift; (void)arm_shift_carry; (void)arm_shift_imm; (void)arm_shift_imm_carry; (void)uqsub8; (void)qsub8; (void)qsub16; (void)par_op; (void)par_sel; (void)guest_udf; (void)guest_unsupported; (void)svc_create_address_arbiter; (void)dq8_pc_needs_boundary;
    (void)branch_indirect; (void)svc_break; (void)svc_arbitrate; (void)svc_send_sync; (void)svc_connect_to_port;
    (void)svc_close_handle; (void)svc_exit_thread; (void)svc_exit_process; (void)svc_get_process_id; (void)svc_create_thread;
    (void)svc_create_mutex; (void)svc_release_mutex; (void)svc_create_event; (void)svc_signal_event; (void)svc_clear_event;
    (void)svc_wait_synchronization; (void)svc_sleep; (void)svc_get_system_tick; (void)svc_duplicate_handle;
    /* SAVESTATE (t_d50c9503): --load-state PATH / --state PATH (env fallbacks
       DQ8_LOAD_STATE / DQ8_STATE_FILE / DQ8_STATE_DIR).  Flags are removed from
       argv so the two positional arguments keep their meaning. */
    const char *load_state = getenv("DQ8_LOAD_STATE");
    if (load_state && !*load_state) load_state = NULL;
    {{
        int w = 1;
        for (int i = 1; i < argc; ++i) {{
            if (!strcmp(argv[i], "--load-state") && i + 1 < argc) {{ load_state = argv[++i]; continue; }}
            if (!strncmp(argv[i], "--load-state=", 13)) {{ load_state = argv[i] + 13; continue; }}
            if (!strcmp(argv[i], "--state") && i + 1 < argc) {{ savestate_set_path(argv[++i]); continue; }}
            if (!strncmp(argv[i], "--state=", 8)) {{ savestate_set_path(argv[i] + 8); continue; }}
            argv[w++] = argv[i];
        }}
        argc = w;
    }}
    if (load_state && *load_state) savestate_set_path(load_state);
    if (argc != 3) {{ fprintf(stderr, "usage: %s DQ8.code DQ8.romfs [--load-state PATH] [--state PATH]\n"
        "env: DQ8_SKIP_MOVIE=1 skips the opening movie (boots straight to the\n"
        "     post-intro main menu); unset plays the intro as usual.\n"
        "     SAVESTATE (t_d50c9503): F5 = save state, F9 = load state in the\n"
        "     SDL window.  --load-state PATH (or DQ8_LOAD_STATE) restores a\n"
        "     state at startup and skips boot.  State slot defaults to\n"
        "     <binary dir>/dq8-state.sav; override with --state PATH,\n"
        "     DQ8_STATE_FILE or DQ8_STATE_DIR.\n", argv[0]); return 2; }}
    romfs_image=fopen(argv[2], "rb"); if(!romfs_image) {{ perror(argv[2]); return 2; }}
    {{
        uint8_t ivfc[0x5Cu];
        if (fread(ivfc, 1, sizeof(ivfc), romfs_image) != sizeof(ivfc) ||
            memcmp(ivfc, "IVFC", 4) != 0 || read_le32(ivfc + 0x54u) != sizeof(ivfc)) {{
            fprintf(stderr, "invalid IVFC RomFS image: %s\n", argv[2]); return 2;
        }}
        uint32_t level3_block_log2 = read_le32(ivfc + 0x4Cu);
        uint64_t level3_logical_offset = read_le64(ivfc + 0x3Cu);
        /* The descriptor stores 64-bit logical offset and size at +0x3C/+0x44.
           ctrtool's extracted image keeps physical levels in hash-tree order:
           for this IVFC layout, Level 3 begins after the 0x1000-byte
           master-hash area, not at its logical offset (0x17CB000). */
        romfs_data_offset = 0x1000u;
        romfs_data_size = read_le64(ivfc + 0x44u);
        if (level3_block_log2 >= 32u || !level3_logical_offset ||
            !romfs_data_offset || !romfs_data_size) {{
            fprintf(stderr, "invalid IVFC Level 3 metadata\n"); return 2;
        }}
        if (fseeko(romfs_image, 0, SEEK_END) || ftello(romfs_image) < 0 ||
            romfs_data_offset > (uint64_t)ftello(romfs_image) ||
            romfs_data_size > (uint64_t)ftello(romfs_image) - romfs_data_offset) {{
            fprintf(stderr, "truncated IVFC Level 3 data\n"); return 2;
        }}
        if (getenv("DQ8_TRACE_FS"))
            fprintf(stderr, "IVFC Level 3 logical=%llX physical=%llX size=%llX block_log2=%u\n",
                    (unsigned long long)level3_logical_offset,
                    (unsigned long long)romfs_data_offset, (unsigned long long)romfs_data_size,
                    level3_block_log2);
        rewind(romfs_image);
    }}
    FILE *f=fopen(argv[1], "rb"); if(!f) {{ perror(argv[1]); return 2; }}
    ram=calloc(1, GUEST_RAM_SIZE); if(!ram) return 2;
    if(fseek(f,0,SEEK_END)||ftell(f)<0) return 2;
    long sz=ftell(f); rewind(f);
    if (0x{base:08X}u + (uint32_t)sz > GUEST_RAM_SIZE || fread(ram+0x{base:08X},1,(size_t)sz,f)!=(size_t)sz) return 2;
    /* The kernel populates this config-memory field from the exheader's
       APPLICATION memory allocation (DQ8 USA is 64 MiB). */
    put32(0x1FF80040u, 0x04000000u);
    const char *trace_pc_text = getenv("DQ8_TRACE_PC");
    if (trace_pc_text) {{
        char trace_pc_buffer[512];
        snprintf(trace_pc_buffer, sizeof(trace_pc_buffer), "%s", trace_pc_text);
        char *save = NULL;
        for (char *part = strtok_r(trace_pc_buffer, ",", &save); part; part = strtok_r(NULL, ",", &save)) {{
            if (trace_guest_pc_count < 16u)
                trace_guest_pcs[trace_guest_pc_count++] = (uint32_t)strtoul(part, NULL, 0);
        }}
    }}
    const char *trace_pc_max_text = getenv("DQ8_TRACE_PC_MAX");
    if (trace_pc_max_text) {{
        trace_guest_pc_max = (unsigned)strtoul(trace_pc_max_text, NULL, 0);
        if (trace_guest_pc_max == 0u) trace_guest_pc_max = 64u;
    }}
    const char *trace_write_text = getenv("DQ8_TRACE_WRITE");
    if (trace_write_text) {{
        char trace_write_buf[512]; char *tw_save = NULL;
        snprintf(trace_write_buf, sizeof trace_write_buf, "%s", trace_write_text);
        for (char *p = strtok_r(trace_write_buf, ",", &tw_save);
             p && trace_write_n < 4u; p = strtok_r(NULL, ",", &tw_save)) {{
            uint32_t a = (uint32_t)strtoul(p, NULL, 0);
            const char *colon = strchr(p, ':');
            uint32_t length = colon ? (uint32_t)strtoul(colon + 1, NULL, 0) : 4u;
            trace_guest_write_addr[trace_write_n] = a;
            trace_guest_write_end[trace_write_n] = a + (length ? length : 4u);
            ++trace_write_n;
        }}
    }}
    if (getenv("DQ8_TRACE_HID_READS")) {{
        trace_hid_reads_enabled = 1;
        trace_hid_t0_ns = host_monotonic_ns();
        fprintf(stderr, "HIDREAD trace enabled t=0 is trace start\n");
    }}
    if (getenv("DQ8_PROGRESS_SECONDS")) {{ signal(SIGALRM, guest_periodic_dump); alarm(5u); }}
    /* GUESTPHASE (t_3de68379): DQ8_GUEST_TIME=1 accumulates the wall clock
       spent inside the guest dispatch loop and reports it on every
       FRAMESTAT line (guest_ms / guest_ms_per_frame / guest_pct_wall). */
    dq8_guest_time_enabled = getenv("DQ8_GUEST_TIME") != NULL;
    dq8_guest_time_t0 = dq8_guest_time_enabled ? host_monotonic_ns() : 0;
    {{ const char *bench_text = getenv("DQ8_BENCH_STEPS");
       if (bench_text && *bench_text) {{
           bench_step_limit = strtoull(bench_text, NULL, 0);
           if (bench_step_limit == 0u) bench_step_limit = 1000000000000ull;
           bench_enabled = 1;
       }}
       {{ const char *nosnap = getenv("DQ8_BENCH_NO_SNAPSHOT");
          if (nosnap && *nosnap && *nosnap != '0') bench_no_snapshot = 1; }}
       bench_t0_ns = host_monotonic_ns(); }}
    /* DETERMINISTIC STEPPING (t_115d297c), OPT-IN.  DQ8_DETERMINISTIC=1
       derives every guest-visible clock from retired guest instructions;
       DQ8_DETERMINISTIC_NS_PER_STEP overrides the mapping (virtual ns per
       retired instruction, default 400 = the measured headless-GL rate of
       ~2.5 M instructions per second).  Unset keeps the frozen behaviour. */
    {{ const char *det = getenv("DQ8_DETERMINISTIC");
       if (det && *det && *det != '0') {{
           const char *det_rate = getenv("DQ8_DETERMINISTIC_NS_PER_STEP");
           dq8_det_mode = 1;
           if (det_rate && *det_rate) {{
               unsigned long long v = strtoull(det_rate, NULL, 0);
               if (v) dq8_det_ns_per_step = (uint64_t)v;
           }}
           dq8_det_steps_total = 0; dq8_det_idle_ns = 0;
           dq8_det_main_reported = 0; dq8_det_next_main_mark = 4096ull;
           atexit(dq8_det_report);
           fprintf(stderr,
                   "DETERMINISTIC: on ns_per_step=%llu (1 vblank per %llu retired "
                   "instructions; guest clock = base + instructions*ns_per_step)\n",
                   (unsigned long long)dq8_det_ns_per_step,
                   (unsigned long long)((GUEST_HOST_POLL_NS + dq8_det_ns_per_step - 1u) / dq8_det_ns_per_step));
       }} }}
    /* S1_BLOCK_GATE */
    /* S1_CFG_GATE */
    host_display_init();
    pica_gl_init();
    uint32_t r[16]={{0}}; r[13]=0x10000000u; r[14]=0xFFFFFFFFu; main_guest_regs=r;
    /* SAVESTATE (t_d50c9503): with --load-state the guest never executes the
       boot sequence — the whole 1 GiB address space, thread/kernel state and
       service bookkeeping are restored from the state file, and the main
       dispatch loop is entered at the saved pc. */
    uint32_t ss_entry = 0x{start:08X}u;
    if (load_state && *load_state) {{
        uint32_t restored_pc = ss_entry;
        if (savestate_cold_load(load_state, &restored_pc, r) == 0) {{
            ss_entry = restored_pc;
            /* DETERMINISTIC (t_115d297c): the saved GuestThread array carries
               absolute host-clock deadlines; rebase them onto the virtual
               clock before the guest resumes. */
            dq8_det_rebase_threads();
            dsp_audio_det_reset_after_load();
        }} else {{
            fprintf(stderr, "SAVESTATE: startup restore failed — booting normally\n");
            memset(r, 0, sizeof r); r[13]=0x10000000u; r[14]=0xFFFFFFFFu;
        }}
    }}
    PF_ENTER(PF_GUEST);
    translated_from(r, ss_entry, NULL);
    PF_EXIT();
    printf("executed statically translated ARM function 0x{start:08X}; r0=0x%08X\n", r[0]);
    host_display_shutdown();
    return 0;
}}
"#));
    // S1b markers: with --s1-block absent these all replace with "" and the
    // generated file stays byte-identical to the pre-S1b output (verified by
    // regenerating the default output and comparing its sha256 against the
    // pre-S1b baseline; see reports/S1B_BLOCK_EMIT.md).
    c = c.replace(
        "/* S1_BLOCK_DEF */\n",
        s1.as_ref().map(|b| b.function_c.as_str()).unwrap_or(""),
    );
    c = c.replace(
        "        /* S1_DISPATCH_HOOK */\n",
        s1.as_ref().map(|b| b.hook_c.as_str()).unwrap_or(""),
    );
    c = c.replace(
        "    /* S1_BLOCK_GATE */\n",
        s1.as_ref().map(|b| b.gate_c.as_str()).unwrap_or(""),
    );
    // S1c markers: same byte-identity contract as S1b's — without --s1-cfg
    // these all replace with "" and the generated C is unchanged.
    c = c.replace(
        "/* S1_CFG_DEF */\n",
        s1_cfg.as_ref().map(|r| r.function_c.as_str()).unwrap_or(""),
    );
    c = c.replace(
        "        /* S1_CFG_HOOK */\n",
        s1_cfg.as_ref().map(|r| r.hook_c.as_str()).unwrap_or(""),
    );
    c = c.replace(
        "    /* S1_CFG_GATE */\n",
        s1_cfg.as_ref().map(|r| r.gate_c.as_str()).unwrap_or(""),
    );
    if let Some(parent) = output.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    fs::write(output, c).map_err(|e| format!("cannot write {}: {e}", output.display()))?;
    println!(
        "Translated {} ARM instructions across {} reachable functions from 0x{start:08X} into {}",
        total_insns,
        functions.len(),
        output.display()
    );
    Ok(())
}

fn run() -> Result<(), String> {
    let mut args = env::args().skip(1);
    let command = args.next().unwrap_or_else(|| usage());
    match command.as_str() {
        "recompile-entry" => {
            let code = args.next().unwrap_or_else(|| usage());
            let output = args.next().unwrap_or_else(|| usage());
            let rest: Vec<String> = args.collect();
            let base = match rest.as_slice() {
                [] => 0x0010_0000,
                [flag, value] if flag == "--base" => {
                    u32::from_str_radix(value.trim_start_matches("0x"), 16)
                        .map_err(|_| "invalid base address".to_string())?
                }
                _ => return Err("recompile-entry accepts optional --base ADDRESS".into()),
            };
            recompile_entry(Path::new(&code), Path::new(&output), base)
        }
        "recompile-arm" => {
            let code = args.next().unwrap_or_else(|| usage());
            let address = args.next().unwrap_or_else(|| usage());
            let output = args.next().unwrap_or_else(|| usage());
            let start = u32::from_str_radix(address.trim_start_matches("0x"), 16)
                .map_err(|_| "invalid ARM function address".to_string())?;
            let rest: Vec<String> = args.collect();
            let mut base = 0x0010_0000;
            let mut extras = Vec::new();
            let mut noreturn_calls = Vec::new();
            let mut s1_block = None;
            let mut s1_cfg_entry: Option<u32> = None;
            let mut s1_cfg_regions: Vec<(u32, u32)> = Vec::new();
            let mut i = 0;
            while i < rest.len() {
                let value = rest
                    .get(i + 1)
                    .ok_or("missing recompile-arm option value")?;
                let parsed = u32::from_str_radix(value.trim_start_matches("0x"), 16)
                    .map_err(|_| format!("invalid address: {value}"))?;
                match rest[i].as_str() {
                    "--base" => base = parsed,
                    "--extra-entry" => extras.push(parsed),
                    "--noreturn-call" => noreturn_calls.push(parsed),
                    "--s1-block" => s1_block = Some(parsed),
                    "--s1-cfg" => s1_cfg_entry = Some(parsed),
                    "--s1-cfg-end" => {
                        let entry = s1_cfg_entry
                            .take()
                            .ok_or("--s1-cfg-end requires --s1-cfg")?;
                        s1_cfg_regions.push((entry, parsed));
                    }
                    _ => {
                        return Err(
                            "recompile-arm options: --base ADDRESS, --extra-entry ADDRESS, --noreturn-call ADDRESS, --s1-block ADDRESS, --s1-cfg ADDRESS --s1-cfg-end ADDRESS".into(),
                        );
                    }
                }
                i += 2;
            }
            // S1c: each region is selected by its entry AND its exclusive
            // end; the rollout passes several --s1-cfg/--s1-cfg-end pairs to
            // convert several regions in one build.
            if s1_cfg_entry.is_some() {
                return Err("--s1-cfg requires --s1-cfg-end".into());
            }
            let s1_cfg = if s1_cfg_regions.is_empty() {
                None
            } else {
                Some(s1_cfg_regions)
            };
            recompile_arm_function_with_noreturn(
                Path::new(&code),
                Path::new(&output),
                base,
                start,
                &extras,
                &noreturn_calls,
                s1_block,
                s1_cfg,
            )
        }
        // S1a: representation-only Block-IR extraction for one guest address
        // range.  Writes the deterministic dump; touches nothing else.
        "extract-blocks" => {
            let code = args.next().unwrap_or_else(|| usage());
            let start = args.next().unwrap_or_else(|| usage());
            let end = args.next().unwrap_or_else(|| usage());
            let start = u32::from_str_radix(start.trim_start_matches("0x"), 16)
                .map_err(|_| "invalid region start address".to_string())?;
            let end = u32::from_str_radix(end.trim_start_matches("0x"), 16)
                .map_err(|_| "invalid region end address".to_string())?;
            let mut base = block_ir::GUEST_BASE;
            let mut output: Option<PathBuf> = None;
            let mut check_roundtrip = false;
            let rest: Vec<String> = args.collect();
            let mut i = 0;
            while i < rest.len() {
                if rest[i] == "--check-roundtrip" {
                    check_roundtrip = true;
                    i += 1;
                    continue;
                }
                let value = rest
                    .get(i + 1)
                    .ok_or("missing extract-blocks option value")?;
                match rest[i].as_str() {
                    "--base" => {
                        base = u32::from_str_radix(value.trim_start_matches("0x"), 16)
                            .map_err(|_| format!("invalid base address: {value}"))?
                    }
                    "--out" => output = Some(PathBuf::from(value)),
                    _ => {
                        return Err(
                            "extract-blocks options: --base ADDRESS, --out FILE, --check-roundtrip"
                                .into(),
                        )
                    }
                }
                i += 2;
            }
            let bytes = fs::read(&code).map_err(|e| format!("cannot read {code}: {e}"))?;
            let region = block_ir::extract_region(&bytes, base, start, end)?;
            let dump = block_ir::dump(&region);
            // --check-roundtrip: parse the canonical dump back and require an
            // identical Region (extract -> dump -> parse -> identical).
            if check_roundtrip {
                let parsed = block_ir::parse(&dump)?;
                if parsed != region {
                    return Err(
                        "block-ir round-trip mismatch: parsed region differs from the extracted one"
                            .into(),
                    );
                }
                if block_ir::dump(&parsed) != dump {
                    return Err("block-ir round-trip mismatch: re-dump differs".into());
                }
                eprintln!(
                    "extract-blocks: round-trip OK ({} blocks parsed back identically)",
                    parsed.blocks.len()
                );
            }
            match output {
                Some(path) => {
                    fs::write(&path, dump.as_bytes())
                        .map_err(|e| format!("cannot write {}: {e}", path.display()))?;
                    eprintln!(
                        "extract-blocks: {} blocks in 0x{:08X}..0x{:08X} -> {}",
                        region.blocks.len(),
                        start,
                        end,
                        path.display()
                    );
                }
                None => print!("{dump}"),
            }
            Ok(())
        }
        "inspect" => {
            let image = args.next().unwrap_or_else(|| usage());
            inspect(Path::new(&image))
        }
        "extract-sarc" => {
            let archive = args.next().unwrap_or_else(|| usage());
            let output = args.next().unwrap_or_else(|| usage());
            if args.next().is_some() {
                return Err("extract-sarc accepts only an archive and output directory".into());
            }
            let count = extract_sarc(Path::new(&archive), Path::new(&output))?;
            println!("Extracted {count} files to {output}");
            Ok(())
        }
        "inspect-layout" => {
            let layout = args.next().unwrap_or_else(|| usage());
            if args.next().is_some() {
                return Err("inspect-layout accepts one layout file".into());
            }
            inspect_bflyt(Path::new(&layout))
        }
        "extract-code" => {
            let image = args.next().unwrap_or_else(|| usage());
            let output = args.next().unwrap_or_else(|| usage());
            let (mut part, mut decompress) = (0usize, false);
            let rest: Vec<String> = args.collect();
            let mut i = 0;
            while i < rest.len() {
                match rest[i].as_str() {
                    "--partition" if i + 1 < rest.len() => {
                        part = rest[i + 1]
                            .parse()
                            .map_err(|_| "invalid partition".to_string())?;
                        i += 2;
                    }
                    "--decompress-blz" => {
                        decompress = true;
                        i += 1;
                    }
                    _ => return Err(format!("unknown argument {}", rest[i])),
                }
            }
            extract_code(Path::new(&image), Path::new(&output), part, decompress)
        }
        "decrypt-code" => {
            let image = args.next().unwrap_or_else(|| usage());
            let output = args.next().unwrap_or_else(|| usage());
            let mut part = 0usize;
            let mut ctrtool = "ctrtool".to_string();
            let mut keyset: Option<PathBuf> = None;
            let rest: Vec<String> = args.collect();
            let mut i = 0;
            while i < rest.len() {
                match rest[i].as_str() {
                    "--partition" if i + 1 < rest.len() => {
                        part = rest[i + 1]
                            .parse()
                            .map_err(|_| "invalid partition".to_string())?;
                        i += 2;
                    }
                    "--ctrtool" if i + 1 < rest.len() => {
                        ctrtool = rest[i + 1].clone();
                        i += 2;
                    }
                    "--keyset" if i + 1 < rest.len() => {
                        keyset = Some(PathBuf::from(&rest[i + 1]));
                        i += 2;
                    }
                    _ => return Err(format!("unknown argument {}", rest[i])),
                }
            }
            decrypt_code_with_ctrtool(
                Path::new(&image),
                Path::new(&output),
                part,
                &ctrtool,
                keyset.as_deref(),
            )
        }
        "decompile" => {
            let code = args.next().unwrap_or_else(|| usage());
            let output = args.next().unwrap_or_else(|| usage());
            let (mut base, mut text_size, mut max_insns) = (0x0010_0000u32, usize::MAX, 128usize);
            let rest: Vec<String> = args.collect();
            let mut i = 0;
            while i < rest.len() {
                match rest[i].as_str() {
                    "--base" if i + 1 < rest.len() => {
                        base = u32::from_str_radix(rest[i + 1].trim_start_matches("0x"), 16)
                            .map_err(|_| "invalid base address".to_string())?;
                        i += 2;
                    }
                    "--text-size" if i + 1 < rest.len() => {
                        text_size = usize::from_str_radix(rest[i + 1].trim_start_matches("0x"), 16)
                            .map_err(|_| "invalid text size".to_string())?;
                        i += 2;
                    }
                    "--max-insns" if i + 1 < rest.len() => {
                        max_insns = rest[i + 1]
                            .parse()
                            .map_err(|_| "invalid instruction limit".to_string())?;
                        i += 2;
                    }
                    _ => return Err(format!("unknown argument {}", rest[i])),
                }
            }
            decompile_reachable(
                Path::new(&code),
                Path::new(&output),
                base,
                text_size,
                max_insns,
            )
        }
        "disasm" => {
            let code = args.next().unwrap_or_else(|| usage());
            let mut mode = "arm".to_string();
            let mut base = 0u32;
            let mut count = 64usize;
            let mut blz = false;
            let rest: Vec<String> = args.collect();
            let mut i = 0;
            while i < rest.len() {
                match rest[i].as_str() {
                    "--mode" if i + 1 < rest.len() => {
                        mode = rest[i + 1].clone();
                        i += 2;
                    }
                    "--base" if i + 1 < rest.len() => {
                        base = u32::from_str_radix(rest[i + 1].trim_start_matches("0x"), 16)
                            .map_err(|_| "invalid base address".to_string())?;
                        i += 2;
                    }
                    "--count" if i + 1 < rest.len() => {
                        count = rest[i + 1]
                            .parse()
                            .map_err(|_| "invalid count".to_string())?;
                        i += 2;
                    }
                    "--decompress-blz" => {
                        blz = true;
                        i += 1;
                    }
                    _ => return Err(format!("unknown argument {}", rest[i])),
                }
            }
            if mode != "arm" && mode != "thumb" {
                return Err("mode must be arm or thumb".into());
            }
            disasm(Path::new(&code), base, &mode, count, blz)
        }
        _ => usage(),
    }
}

fn main() {
    if let Err(e) = run() {
        eprintln!("dq8tool: {e}");
        process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn direct_arm_and_interworking_branch_targets_match_disassembler() {
        assert_eq!(
            arm_branch_target(0xEB00_0007, 0x0010_0000),
            Some((0x0010_0024, true, false))
        );
        assert_eq!(
            arm_branch_target(0xFA00_0CF1, 0x0010_0010),
            Some((0x0010_33DC, true, false))
        );
    }

    #[test]
    fn thumb_blx_uses_the_word_aligned_pc_and_low_immediate_bits() {
        // This instruction at 0x002FF492 is BLX 0x002FD70C.
        assert_eq!(
            thumb_bl_target(0x002F_F492, 0xF7FE, 0xE93C),
            Some((0x002F_D70C, true))
        );
    }

    #[test]
    fn startup_constant_return_veneers_are_recognized() {
        let mut simple = vec![0u8; 12];
        simple[0..4].copy_from_slice(&0xE59F_0000u32.to_le_bytes());
        simple[4..8].copy_from_slice(&0xE12F_FF1Eu32.to_le_bytes());
        simple[8..12].copy_from_slice(&0x0061_3800u32.to_le_bytes());
        assert_eq!(
            arm_constant_return(&simple, 0x1000, 0x1000, false),
            Some(0x0061_3800)
        );

        let mut null_string = vec![0u8; 40];
        for (i, word) in [
            0xE92D_4010u32,
            0xE351_0000,
            0x0A00_0001,
            0,
            0,
            0xE59F_0008,
            0xE08F_0000,
            0xE8BD_8010,
            0,
            0x0000_2000,
        ]
        .iter()
        .enumerate()
        {
            null_string[i * 4..i * 4 + 4].copy_from_slice(&word.to_le_bytes());
        }
        assert_eq!(
            arm_constant_return(&null_string, 0x1000, 0x1000, true),
            Some(0x3020)
        );
        assert_eq!(
            arm_constant_return(&null_string, 0x1000, 0x1000, false),
            None
        );
    }

    #[test]
    fn thumb_blx_register_is_an_indirect_call() {
        assert_eq!(decode_thumb(0x4780, 0x0010_33EC), "call_indirect(r0);");
        assert_eq!(decode_thumb(0x4770, 0x0010_33F4), "return;");
        assert_eq!(decode_thumb(0x447C, 0x0010_33E0), "r4 += 0x001033E4;");
        assert_eq!(decode_thumb(0x4C06, 0x0010_33DE), "r4 = *(0x001033F8);");
    }

    #[test]
    fn arm_condition_and_postindexed_store_are_preserved() {
        let text = decode_arm(0x3480_2004, 0x0010_0034);
        assert!(text.contains("if (cc)"));
        assert!(text.contains("*(r0) = r2;"));
        assert!(text.contains("r0 += 0x4;"));
    }

    #[test]
    fn recompiler_snapshots_arm_sources_before_setting_arithmetic_flags() {
        let stem = format!("dq8-arm-flags-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = Vec::new();
        code.extend_from_slice(&0xE0B2_2002u32.to_le_bytes()); // ADCS r2,r2,r2
        code.extend_from_slice(&0xE12F_FF1Eu32.to_le_bytes()); // BX lr
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("_dp_a=r[2], _dp_b=r[2], _dp_cin=(uint32_t)c"));
        assert!(generated.contains("_dp_result=(_dp_a + _dp_b + _dp_cin); r[2]=_dp_result;"));
        assert!(generated.contains("wide=(uint64_t)a+b+_dp_cin"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn thumb_add_sub_immediate_sets_carry_and_overflow() {
        let stem = format!("dq8-thumb-arith-flags-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = Vec::new();
        code.extend_from_slice(&0xE12F_FF1Eu32.to_le_bytes()); // ARM BX lr
        code.extend_from_slice(&0x1E64u16.to_le_bytes()); // Thumb SUBS r4, r4, #1
        code.extend_from_slice(&0x4770u16.to_le_bytes()); // Thumb BX lr
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[0x1005]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("c=(a>=b); v=(((a^b)&(a^t))>>31)"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn arm_nop_hint_prologue_overrides_thumb_halfword_heuristic() {
        for first in [0xE320_F000u32, 0xE1A0_0000, 0xF57F_F01F, 0xE51F_F004] {
            let mut code = Vec::new();
            code.extend_from_slice(&first.to_le_bytes());
            code.extend_from_slice(&0xEB00_0000u32.to_le_bytes()); // BL
            code.extend_from_slice(&0xE12F_FF1Eu32.to_le_bytes()); // BX lr
            assert_eq!(
                looks_like_thumb_entry(&code, 0x1000, 0x1000),
                matches!(first, 0xE320_F000 | 0xF57F_F01F),
                "first instruction 0x{first:08X}"
            );
            assert!(looks_like_arm_entry(&code, 0x1000, 0x1000));
        }
    }

    #[test]
    fn armv6_nop_hint_preserves_flags_for_following_conditional_branch() {
        assert_eq!(decode_arm(0xE320_F000, 0x1000), "nop;");

        let stem = format!("dq8-armv6-nop-flags-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = Vec::new();
        code.extend_from_slice(&0xE257_7001u32.to_le_bytes()); // SUBS r7,r7,#1
        code.extend_from_slice(&0xE320_F000u32.to_le_bytes()); // NOP hint
        code.extend_from_slice(&0x1AFF_FFFCu32.to_le_bytes()); // BNE 0x1000
        code.extend_from_slice(&0xE12F_FF1Eu32.to_le_bytes()); // BX lr
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        let nop_case = generated
            .lines()
            .find(|line| line.contains("case 0x00001004:"))
            .unwrap();
        assert!(nop_case.contains("ARMv6 NOP hint"), "{nop_case}");
        assert!(!nop_case.contains("z ="), "NOP changed flags: {nop_case}");
        assert!(generated.contains("case 0x00001008: { if (!z)"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn conditional_ldm_to_pc_keeps_the_sequential_fallthrough_translated() {
        let stem = format!("dq8-conditional-ldm-fallthrough-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = Vec::new();
        code.extend_from_slice(&0xE92D_4010u32.to_le_bytes()); // PUSH {r4,lr}
        code.extend_from_slice(&0x08BD_8010u32.to_le_bytes()); // LDMEQIA sp!,{r4,pc}
        code.extend_from_slice(&0xE320_F000u32.to_le_bytes()); // fallthrough NOP
        code.extend_from_slice(&0xE12F_FF1Eu32.to_le_bytes()); // BX lr
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("case 0x00001004: { if (z)"));
        assert!(generated.contains("else { pc = 0x00001008; }"));
        assert!(generated.contains("case 0x00001008:"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn known_context_restore_calls_do_not_translate_inline_data_as_code() {
        let stem = format!("dq8-noreturn-call-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = Vec::new();
        code.extend_from_slice(&0xEB00_0002u32.to_le_bytes()); // BL 0x1010
        code.extend_from_slice(&0xE50F_0004u32.to_le_bytes()); // inline data; invalid STR with PC base
        code.extend_from_slice(&0u32.to_le_bytes());
        code.extend_from_slice(&0u32.to_le_bytes());
        code.extend_from_slice(&0xE12F_FF1Eu32.to_le_bytes()); // context restore's BX LR
        fs::write(&input, code).unwrap();

        // Without the noreturn list the inline data after the BL IS decoded:
        // it must stop its block with a loud reject - never lowered as a store
        // (R2-01) and never an Err that fails sibling roots.  The old contract
        // (`recompile_arm_function(...).is_err()`) was retired in round-17:
        // ASCII rodata that merely shapes like `str rN, [pc, #imm]` (0x00571F5C
        // = "thod" in "Method_Init\0Method_Output\0...") aborted the whole
        // `recompile-arm` run and made every root unbuildable.
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("guest_unsupported(0x00001004u, 0xE50F0004u, 0u);"));
        let case_1004 = generated
            .lines()
            .find(|l| l.contains("case 0x00001004:"))
            .unwrap_or("");
        assert!(!case_1004.contains("put32"));
        // Pre-existing compile break (HEAD, not introduced here): the S1
        // rollout widened `recompile_arm_function_with_noreturn` with
        // --s1-block/--s1-cfg but this test call was never updated, which
        // failed the whole `cargo test` gate before it could run a single
        // test.  Pass the "no S1 emission" defaults so the suite compiles.
        recompile_arm_function_with_noreturn(
            &input, &output, 0x1000, 0x1000, &[], &[0x1010], None, None,
        )
            .unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("case 0x00001000:"));
        assert!(generated.contains("case 0x00001010:"));
        assert!(!generated.contains("case 0x00001004:"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn recompiler_queues_the_thumb_target_of_pc_relative_ldr_veneers() {
        let stem = format!("dq8-recomp-ldr-pc-veneer-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = vec![0u8; 0x14];
        code[0..4].copy_from_slice(&0xE51F_F004u32.to_le_bytes()); // LDR pc,[pc,#-4]
        code[4..8].copy_from_slice(&0x1011u32.to_le_bytes()); // Thumb target
        code[0x10..0x12].copy_from_slice(&0x4770u16.to_le_bytes()); // BX lr
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("case 0x00001000:"));
        assert!(generated.contains("case 0x00001011:"));
        assert!(!generated.contains("case 0x00001004:"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn arm_ldrexd_strexd_keep_the_exclusive_doubleword_size() {
        let stem = format!("dq8-arm-exclusive-double-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xE1BC_0F9Fu32, 0xE1AC_0F92u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("exclusive_size = 8u; r[0] = mem32(exclusive_addr)"));
        assert!(generated.contains("exclusive_size == 8u"));
        assert!(generated.contains("put32(r[12] + 4u, r[3])"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_vabs_clears_the_sign_bit() {
        let stem = format!("dq8-vfp-vabs-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEEB0_0AC8u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("fpr[0] = fpr[16] & 0x7FFFFFFFu"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_vmov_f64_copies_both_words_of_the_double_register() {
        // vmov.f64 d8, d0 (0xEEB08B40): sz=1 -> F64 copy of the word pair.
        // d8 = fpr[16],fpr[17]; d0 = fpr[0],fpr[1].
        let stem = format!("dq8-vfp-vmov-f64-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEEB0_8B40u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("fpr[16] = fpr[0]; fpr[17] = fpr[1];"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_vldr_f64_loads_both_words_of_the_double_register() {
        // vldr d0, [pc, #44] (0xED9F0B0B): sz=1 -> double -> two-word load.
        // d0 = fpr[0] (low) and fpr[1] (high, at ea+4).
        let stem = format!("dq8-vfp-vldr-f64-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xED9F_0B0Bu32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("fpr[0] = mem32("));
        assert!(generated.contains("fpr[1] = mem32("));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_vldr_f64_out_of_bank_register_is_rejected_explicitly() {
        // vldr d16, [pc, #0] (0xEDDF0B00): bit22=1, bits[15:12]=0 -> Vd=16,
        // off the 16-double bank behind fpr[32] - loud reject, never silent.
        let stem = format!("dq8-vfp-vldr-f64-d16-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEDDF_0B00u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        // The reject must fire for THIS instruction's case (positive proof the
        // d16 out-of-bank path was taken instead of a silent load).
        assert!(generated.contains(
            "case 0x00001000: { guest_unsupported(0x00001000u, 0xEDDF0B00u, 0u);"
        ));
        // And that case must not also emit an fpr load/store (the silent path).
        let case_line = generated
            .lines()
            .find(|l| l.contains("case 0x00001000:"))
            .unwrap_or("");
        assert!(!case_line.contains("fpr["));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_negated_multiply_accumulate_forms_preserve_the_destination_operand() {
        let stem = format!("dq8-vnmla-vnmls-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEE11_1AC0u32, 0xEE11_1A80u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(
            generated.contains(
                "-(f32_from_bits(fpr[2]) + f32_from_bits(fpr[3]) * f32_from_bits(fpr[0]))"
            )
        );
        assert!(
            generated.contains(
                "-(f32_from_bits(fpr[2]) - f32_from_bits(fpr[3]) * f32_from_bits(fpr[0]))"
            )
        );
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_square_root_handles_single_and_double_precision() {
        let stem = format!("dq8-vsqrt-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEEF1_2AE0u32, 0xEEB1_5BC1u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("fpr[5] = f32_to_bits(sqrtf(f32_from_bits(fpr[1])))"));
        assert!(generated.contains("f64_to_bits(sqrt(f64_from_bits(_src)))"));
        // _src must read the SOURCE pair d1 = (fpr[2], fpr[3]); the old
        // emission read the destination's high word (fpr[11]) instead.
        assert!(generated.contains("_src=((uint64_t)fpr[3] << 32) | fpr[2]"));
        assert!(generated.contains("fpr[10]=(uint32_t)_result; fpr[11]=(uint32_t)(_result >> 32)"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vcvt_between_f32_and_f64_decodes_both_directions() {
        let stem = format!("dq8-vcvt-fpfp-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // 0xEEB70BC0 = `vcvt.f32.f64 s0, d0` - the round-17 wall opcode that
        // the task card filed as VMRS (llvm-objdump decodes it as the
        // double->single conversion; VMRS r0, FPSCR is 0xEEF10A10).
        // 0xEEB7CACC = `vcvt.f64.f32 d12, s24`: destination pair (fpr[24],
        // fpr[25]) aliases the source single word (fpr[24]).
        let code = [0xEEB7_0BC0u32, 0xEEB7_CACCu32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains(
            "fpr[0] = f32_to_bits((float)f64_from_bits(((uint64_t)fpr[1] << 32) | fpr[0]));"
        ));
        assert!(generated.contains("f64_to_bits((double)f32_from_bits(fpr[24]))"));
        assert!(generated
            .contains("fpr[24] = (uint32_t)_f64r; fpr[25] = (uint32_t)(_f64r >> 32)"));
        // Neither wall opcode may take the reject path.
        assert!(!generated.contains("guest_unsupported(0x00001000u, 0xEEB70BC0u"));
        assert!(!generated.contains("guest_unsupported(0x00001004u, 0xEEB7CACCu"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vcvt_f32_f64_rejects_an_out_of_bank_double_source() {
        let stem = format!("dq8-vcvt-fpfp-oob-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // `vcvt.f32.f64 s0, d30`: Dm = 30 sits behind the 16-double bank
        // (fpr[60] would alias fpscr) - must reject loudly, not lower.
        let code = [0xEEB7_0BEEu32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("guest_unsupported(0x00001000u, 0xEEB70BEEu, 0u);"));
        let case_line = generated
            .lines()
            .find(|l| l.contains("case 0x00001000:"))
            .unwrap_or("");
        assert!(!case_line.contains("fpr["));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vcvt_double_integer_forms_mirror_the_single_precision_arms() {
        let stem = format!("dq8-vcvt-int-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // 0xEEB80BC0 `vcvt.f64.s32 d0, s0`, 0xEEB80B40 `vcvt.f64.u32 d0, s0`,
        // 0xEEBD0BC0 `vcvt.s32.f64 s0, d0`, 0xEEBC0BC0 `vcvt.u32.f64 s0, d0`:
        // the double-precision twins of the single forms already handled.
        let code = [0xEEB8_0BC0u32, 0xEEB8_0B40u32, 0xEEBD_0BC0u32, 0xEEBC_0BC0u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("f64_to_bits((double)(int32_t)fpr[0])"));
        assert!(generated.contains("f64_to_bits((double)(uint32_t)fpr[0])"));
        assert!(generated.contains(
            "fpr[0] = (uint32_t)(int32_t)f64_from_bits(((uint64_t)fpr[1] << 32) | fpr[0]);"
        ));
        assert!(generated
            .contains("fpr[0] = (uint32_t)f64_from_bits(((uint64_t)fpr[1] << 32) | fpr[0]);"));
        assert!(!generated.contains("guest_unsupported(0x0000"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vmrs_vmsr_fpexc_siblings_are_handled() {
        let stem = format!("dq8-vmrs-fpexc-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // 0xEEF80A10 `vmrs r0, fpexc`, 0xEEE80A10 `vmsr fpexc, r0`: the
        // FPSCR pair was already handled, these siblings were not.
        let code = [0xEEF8_0A10u32, 0xEEE8_0A10u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("r[0] = 0x40000000u;"));
        assert!(generated.contains("VMSR FPEXC: VFP permanently enabled here, write dropped"));
        assert!(!generated.contains("guest_unsupported(0x00001000u, 0xEEF80A10u"));
        assert!(!generated.contains("guest_unsupported(0x00001004u, 0xEEE80A10u"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn negative_offset_vldr_vstr_use_the_immediate_form_not_the_list_count() {
        let stem = format!("dq8-vstr-neg-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // 0xED408ACA `vstr s17, [r0, #-808]` - esc30's wall at pc=0x00220138.
        // P=1, U=0, W=0 makes P != U, so the old multiple-transfer guard
        // claimed it and read imm8=202 as a register COUNT (> 32 -> reject)
        // instead of an offset.  0xED500A01 `vldr s1, [r0, #-4]` is the load
        // twin.  0xECA00A04 `vstmia r0!, {s0-s3}` (W=1) and 0xEC800A03
        // `vstmia r0, {s0-s2}` (P=0, W=0 - the no-writeback list form llvm
        // accepts) must BOTH still take the list path: P=1 && W=0 is the
        // discriminator, not W alone.
        let code = [
            0xED40_8ACAu32,
            0xED50_0A01u32,
            0xECA0_0A04u32,
            0xEC80_0A03u32,
            0xE12F_FF1Eu32,
        ]
        .into_iter()
        .flat_map(u32::to_le_bytes)
        .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("put32((r[0] - 808u), fpr[17]);"));
        assert!(generated.contains("fpr[1] = mem32((r[0] - 4u));"));
        assert!(generated.contains("put32(a + 0u, fpr[0]);"));
        assert!(generated.contains(" r[0] = r[0] + 16u;"));
        // no-writeback list form: three words, no base update
        assert!(generated.contains("put32(a + 8u, fpr[2]);"));
        assert!(!generated.contains("guest_unsupported(0x0000"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vmov_two_register_form_moves_a_double_to_and_from_core_registers() {
        let stem = format!("dq8-vmov2reg-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // 0xEC532B10 `vmov r2, r3, d0` (@0x0016F5E8) and 0xEC510B18
        // `vmov r0, r1, d8` (@0x002FFDC0) are lifted-but-rejecting sites;
        // 0xEC410B10 is the core -> VFP direction; 0xEC510B3E is
        // `vmov r0, r1, d30`, whose Dm is behind the 16-double bank.
        let code = [
            0xEC53_2B10u32,
            0xEC51_0B18u32,
            0xEC41_0B10u32,
            0xEC51_0B3Eu32,
            0xE12F_FF1Eu32,
        ]
        .into_iter()
        .flat_map(u32::to_le_bytes)
        .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("r[2] = fpr[0]; r[3] = fpr[1];"));
        assert!(generated.contains("r[0] = fpr[16]; r[1] = fpr[17];"));
        assert!(generated.contains("fpr[0] = r[0]; fpr[1] = r[1];"));
        assert!(generated.contains("guest_unsupported(0x0000100Cu, 0xEC510B3Eu, 0u);"));
        assert!(!generated.contains("guest_unsupported(0x00001000u"));
        assert!(!generated.contains("guest_unsupported(0x00001004u"));
        assert!(!generated.contains("guest_unsupported(0x00001008u"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_push_one_double_uses_eight_bytes() {
        let stem = format!("dq8-vfp-vpush-double-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // VPUSH {d8}; the list length is two 32-bit words, not four.
        let code = [0xED2D_8B02u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("uint32_t a=r[13] - 8u;put32(a + 0u, fpr[16]);put32(a + 4u, fpr[17]); r[13] = r[13] - 8u;"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_unsigned_conversions_preserve_the_unsigned_operation() {
        let stem = format!("dq8-vfp-unsigned-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEEB8_0A48u32, 0xEEFC_0AC0u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("fpr[0] = f32_to_bits((float)(uint32_t)fpr[16])"));
        assert!(generated.contains("fpr[1] = (uint32_t)f32_from_bits(fpr[0])"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_compare_updates_fpscr_for_single_and_double_values() {
        let stem = format!("dq8-vfp-compare-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEEB4_8AC1u32, 0xEEB4_8BC2u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("f32_is_nan_bits(_a_bits) || f32_is_nan_bits(_b_bits)"));
        assert!(generated.contains("f64_from_bits(_a_bits)"));
        assert!(generated.contains("((uint64_t)fpr[17] << 32) | fpr[16]"));
        assert!(generated.contains("0x30000000u : (_a == _b"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_f64_arithmetic_uses_double_pairs_not_f32_lowering() {
        let stem = format!("dq8-vfp-f64-arith-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // VADD.F64 d0,d1,d2 (the audit R2-01 counterexample opcode
        // 0xEE310B02) followed by VADD.F32 s0,s1,s2, which must keep the
        // original single-precision emission byte-for-byte.
        let code = [0xEE31_0B02u32, 0xEE30_0A81u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains(
            "{ uint64_t _f64r = f64_to_bits(f64_from_bits(((uint64_t)fpr[3] << 32) | fpr[2]) + f64_from_bits(((uint64_t)fpr[5] << 32) | fpr[4])); fpr[0] = (uint32_t)_f64r; fpr[1] = (uint32_t)(_f64r >> 32); }"
        ));
        assert!(!generated
            .contains("fpr[0] = f32_to_bits(f32_from_bits(fpr[2]) + f32_from_bits(fpr[4]));"));
        assert!(generated
            .contains("fpr[0] = f32_to_bits(f32_from_bits(fpr[1]) + f32_from_bits(fpr[2]));"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_f64_mac_and_div_families_keep_pair_semantics() {
        let stem = format!("dq8-vfp-f64-mac-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // VMLA.F64 d0,d1,d2; VNMLS.F64 d1,d2,d3; VDIV.F64 d4,d5,d6.
        let code = [0xEE01_0B02u32, 0xEE12_1B03u32, 0xEE85_4B06u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        // VMLA.F64: accumulate d0 += d1 * d2 through the pair path.
        assert!(generated.contains(
            "f64r = f64_to_bits(f64_from_bits(((uint64_t)fpr[1] << 32) | fpr[0]) + f64_from_bits(((uint64_t)fpr[3] << 32) | fpr[2]) * f64_from_bits(((uint64_t)fpr[5] << 32) | fpr[4])); fpr[0] = (uint32_t)_f64r; fpr[1] ="
        ));
        // VNMLS.F64: -(d - n * m), bit6=0 selects the negated-MLS sign.
        assert!(generated.contains(
            "-(f64_from_bits(((uint64_t)fpr[3] << 32) | fpr[2]) - f64_from_bits(((uint64_t)fpr[5] << 32) | fpr[4]) * f64_from_bits(((uint64_t)fpr[7] << 32) | fpr[6]))"
        ));
        // VDIV.F64: both operands and the destination are word pairs.
        assert!(generated.contains(
            "f64_from_bits(((uint64_t)fpr[11] << 32) | fpr[10]) / f64_from_bits(((uint64_t)fpr[13] << 32) | fpr[12])); fpr[8] = (uint32_t)_f64r; fpr[9] = (uint32_t)(_f64r >> 32);"
        ));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn vfp_f64_out_of_bank_register_is_rejected_explicitly() {
        let stem = format!("dq8-vfp-f64-d16-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // VADD.F64 d16,d1,d2: D:Vd=16 is undefined on the 16-double bank
        // behind fpr[32] (index 32 would alias fpscr).  Audit R2-01 accepts
        // only a loud rejection here - never silent execution.
        let code = [0xEE71_0B02u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("guest_unsupported(0x00001000u, 0xEE710B02u, 0u);"));
        assert!(!generated.contains("_f64r"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn extra_thumb_roots_keep_their_interworking_bit() {
        let stem = format!("dq8-thumb-root-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = vec![0u8; 8];
        code[0..4].copy_from_slice(&0xE12F_FF1Eu32.to_le_bytes()); // BX lr
        code[4..6].copy_from_slice(&0x4770u16.to_le_bytes()); // BX lr
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[0x1005]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains("case 0x00001005:"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn condition_wrap_drops_the_else_arm_only_for_a_statically_true_condition() {
        // Unconditional (cond 14/15): body only -- no second `pc` store.
        assert_eq!(condition_wrap("1", "r[0] = 1u;", "pc = 0x4;"), "r[0] = 1u;");
        // Conditional: the failed-condition path must still continue at the
        // fall-through address.
        assert_eq!(
            condition_wrap("z", "r[0] = 1u;", "pc = 0x4;"),
            "if (z) { r[0] = 1u; } else { pc = 0x4; }"
        );
    }

    #[test]
    fn unconditionally_executed_instructions_are_not_wrapped_in_a_dead_else_arm() {
        // MOV r0, #1  (0xE3A00001, AL) -- must be emitted unwrapped.
        // MOVEQ r1,#2 (0x03A01002, EQ) -- must keep its condition wrapper.
        // BX lr.
        let stem = format!("dq8-uncond-no-wrapper-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xE3A0_0001u32, 0x03A0_1002u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(!generated.contains("if (1)"), "{generated}");
        let case_at = |pc: u32| {
            generated
                .lines()
                .find(|l| l.contains(&format!("case 0x{pc:08X}:")))
                .unwrap_or("")
                .to_string()
        };
        let al = case_at(0x1000);
        assert!(!al.contains("if ("), "unconditional case kept a wrapper: {al}");
        assert!(al.contains("pc = 0x00001004;"), "lost the fall-through pc: {al}");
        let eq = case_at(0x1004);
        assert!(eq.contains("if (z) {"), "conditional case lost its guard: {eq}");
        assert!(
            eq.contains("else { pc = 0x00001008; }"),
            "conditional case lost its fall-through arm: {eq}"
        );
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn thumb_add_rd_pc_reads_pc_plus_four_unaligned() {
        // Thumb ADD Rd, PC (0x4400 group, Rm=15): the PC read is instr+4 and is
        // NOT aligned to 4 (Align(PC+4,4) applies only to LDR-literal/ADR).
        // The off-by-2 only shows when the ADD sits at addr mod 4 == 2:
        //   0x1004 movs r0, #0   (0x2000)
        //   0x1006 add  r3, pc   (0x447B)  -> r3 += 0x100A  (never 0x1008)
        //   0x1008 bx    lr      (0x4770)
        let stem = format!("dq8-thumb-add-pc-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = vec![0u8; 10];
        code[0..2].copy_from_slice(&0x2000u16.to_le_bytes()); // padding @0x1000
        code[2..4].copy_from_slice(&0x2000u16.to_le_bytes()); // padding @0x1002
        code[4..6].copy_from_slice(&0x2000u16.to_le_bytes()); // movs r0,#0 @0x1004
        code[6..8].copy_from_slice(&0x447Bu16.to_le_bytes()); // add r3,pc @0x1006
        code[8..10].copy_from_slice(&0x4770u16.to_le_bytes()); // bx lr @0x1008
        fs::write(&input, code).unwrap();
        // Odd start forces Thumb and lands the function at 0x1004 (aligned),
        // so the ADD at 0x1006 is decoded at addr mod 4 == 2.
        recompile_arm_function(&input, &output, 0x1000, 0x1005, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        // PC read = 0x1006+4 = 0x100A (2 mod 4 preserved), not Align(..,4)=0x1008.
        assert!(generated.contains("r[3] += 0x0000100Au;"));
        assert!(!generated.contains("r[3] += 0x00001008u;"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn thumb2_vfp_transfer_delegates_to_the_a32_chain_and_returns_to_thumb() {
        // Thumb-2 `vstr s0, [sp, #8]` = hw1 0xED8D, hw2 0x0A02 (A32 equivalent
        // 0xED8D0A02, which the A32 VLDR/VSTR arm lowers).  The Thumb path has
        // no coprocessor arm of its own, so it redirects into a forced A32 twin
        // decoded from the halfword-swapped word, then a sentinel at the next
        // 4-byte boundary hands control back to Thumb state (t_4734a0a2).
        let stem = format!("dq8-thumb2-vfp-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = vec![0u8; 8];
        code[0..2].copy_from_slice(&0xED8Du16.to_le_bytes()); // vstr s0,[sp,#8] hw1
        code[2..4].copy_from_slice(&0x0A02u16.to_le_bytes()); //                hw2
        code[4..6].copy_from_slice(&0x4770u16.to_le_bytes()); // bx lr @0x1004
        code[6..8].copy_from_slice(&0x46C0u16.to_le_bytes()); // padding
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1001, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        // Thumb case (thumb bit set) only jumps into its A32 twin...
        assert!(generated.contains("case 0x00001001: { pc = 0x00001000;"));
        // ...the twin lowers the actual VFP store with the A32 arm's semantics
        // (s0 -> fpr[0], sp-relative, no writeback) and falls through to the
        // sentinel address in A32 state...
        assert!(generated.contains(
            "case 0x00001000: { put32((r[13] + 8u), fpr[0]); pc = 0x00001004;"
        ));
        // ...which returns to Thumb state at the address with the thumb bit set.
        assert!(generated.contains("case 0x00001004: { pc = 0x00001005u;"));
        // No reject was emitted for the delegated instruction.
        assert!(!generated.contains("guest_unsupported(0x00001000u, 0xED8D0A02u"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn thumb2_non_vfp_coprocessor_number_is_rejected_with_the_full_word() {
        // hw1 0xED8D with hw2 0xF592: coprocessor number 5 is neither VFP (10)
        // nor anything this recompiler implements (no Advanced-SIMD arm), so the
        // reject must carry the whole 32-bit encoding, not just hw1 - the old
        // fall-through printed only 0xED8D.
        let stem = format!("dq8-thumb2-nonvfp-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = vec![0u8; 8];
        code[0..2].copy_from_slice(&0xED8Du16.to_le_bytes());
        code[2..4].copy_from_slice(&0xF592u16.to_le_bytes());
        code[4..6].copy_from_slice(&0x4770u16.to_le_bytes());
        code[6..8].copy_from_slice(&0x46C0u16.to_le_bytes());
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1001, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains(
            "guest_unsupported(0x00001000u, 0xED8DF592u, 1u);"
        ));
        assert!(!generated.contains("guest_unsupported(0x00001000u, 0xED8Du"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn thumb2_pc_relative_vfp_literal_is_rejected_not_guess_a_base() {
        // Thumb-2 `vldr s0, [pc, #0]` (hw1 0xED9F, hw2 0x0A00).  Delegating it
        // would let the A32 arm take the literal base as addr+8 while Thumb-2
        // aligns addr+4 - a silent wrong load (R2-01 forbids), so it must stop
        // the block loudly instead.
        let stem = format!("dq8-thumb2-pcvldr-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let mut code = vec![0u8; 8];
        code[0..2].copy_from_slice(&0xED9Fu16.to_le_bytes());
        code[2..4].copy_from_slice(&0x0A00u16.to_le_bytes());
        code[4..6].copy_from_slice(&0x4770u16.to_le_bytes());
        code[6..8].copy_from_slice(&0x46C0u16.to_le_bytes());
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1001, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains(
            "guest_unsupported(0x00001000u, 0xED9F0A00u, 1u);"
        ));
        assert!(!generated.contains("pc = 0x00001000;"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn resource_limit_svcs_use_the_3ds_limit_and_current_value_abi() {
        let stem = format!("dq8-resource-limit-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        let code = [0xEF00_0039u32, 0xEF00_003Au32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(
            generated.contains("case 0x00001000: { r[0] = svc_get_resource_values(r, 1);")
        );
        assert!(
            generated.contains("case 0x00001004: { r[0] = svc_get_resource_values(r, 0);")
        );
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn indirect_tailcalls_are_distinguished_from_returns() {
        let arm = 0xE8BD_4010u32.to_le_bytes();
        let mut thumb = vec![0u8; 8];
        thumb[0..2].copy_from_slice(&0xBC08u16.to_le_bytes());
        thumb[2..4].copy_from_slice(&0xB004u16.to_le_bytes());
        assert!(arm_restores_lr_before_bx(&arm, 4));
        assert_eq!(
            arm_pc_relative_bx_target(&[0x01, 0xC0, 0x8F, 0xE2, 0, 0, 0, 0], 4, 0x0010_0004, 12),
            Some(0x0010_0009)
        );
        assert!(thumb_restores_stack_before_bx(&thumb, 4));
        assert!(!thumb_restores_stack_before_bx(&thumb, 0));
    }

    #[test]
    fn arm_load_to_pc_ends_the_basic_block() {
        let base = 0x1000;
        let words = [0xE92D_4010u32, 0xE49D_F004, 0xE1A0_0000];
        let data: Vec<u8> = words.into_iter().flat_map(u32::to_le_bytes).collect();
        let (lines, _) = trace_function(
            &data,
            base,
            base + data.len() as u32,
            FuncKey {
                addr: base,
                mode: ArmMode::Arm,
            },
            16,
        );
        assert_eq!(lines.len(), 2);
        assert!(lines.get(&(base + 4)).unwrap().contains("return_indirect"));
    }

    #[test]
    fn arm_prefetch_hint_is_not_a_load_to_pc_branch() {
        let base = 0x1000;
        let words = [0xF5D1_F040u32, 0xE1A0_0000, 0xE12F_FF1E];
        let data: Vec<u8> = words.into_iter().flat_map(u32::to_le_bytes).collect();
        let (lines, _) = trace_function(
            &data,
            base,
            base + data.len() as u32,
            FuncKey {
                addr: base,
                mode: ArmMode::Arm,
            },
            16,
        );
        assert!(decode_arm(0xF5D1_F040, base).contains("prefetch [r1+0x40]"));
        assert!(lines.get(&base).unwrap().contains("prefetch"));
        assert!(lines.contains_key(&(base + 4)));
        assert!(lines.get(&(base + 8)).unwrap().contains("return"));
    }

    #[test]
    fn arm_clrex_is_not_a_load_to_pc_branch() {
        let base = 0x1000;
        let words = [0xF57F_F01Fu32, 0xE1A0_0000, 0xE12F_FF1E];
        let data: Vec<u8> = words.into_iter().flat_map(u32::to_le_bytes).collect();
        let (lines, _) = trace_function(
            &data,
            base,
            base + data.len() as u32,
            FuncKey {
                addr: base,
                mode: ArmMode::Arm,
            },
            16,
        );
        assert_eq!(decode_arm(0xF57F_F01F, base), "clrex;");
        assert!(lines.contains_key(&(base + 4)));
        assert!(lines.get(&(base + 8)).unwrap().contains("return"));
    }

    #[test]
    fn arm_pc_relative_ldr_pc_resolves_literal_veneer_targets() {
        let base = 0x1000;
        let mut data = vec![0u8; 16];
        // At base, LDR pc,[pc,#-4] reads the word at base+4.
        data[0..4].copy_from_slice(&0xE51F_F004u32.to_le_bytes());
        data[4..8].copy_from_slice(&0x2001u32.to_le_bytes());
        assert_eq!(
            arm_pc_relative_ldr_target(&data, base, base, 0xE51F_F004),
            Some(0x2001)
        );
    }

    #[test]
    fn arm_ldr_pc_queues_thumb_target_without_tracing_it_as_arm() {
        let base = 0x1000u32;
        let mut data = vec![0u8; 0x100];
        data[0..4].copy_from_slice(&0xE51F_F004u32.to_le_bytes());
        data[4..8].copy_from_slice(&(base + 0x81).to_le_bytes());
        data[0x80..0x82].copy_from_slice(&0x4770u16.to_le_bytes());
        let (lines, calls) = trace_function(
            &data,
            base,
            base + data.len() as u32,
            FuncKey {
                addr: base,
                mode: ArmMode::Arm,
            },
            16,
        );
        assert_eq!(lines.len(), 1);
        assert!(calls.contains(&FuncKey {
            addr: base + 0x80,
            mode: ArmMode::Thumb,
        }));
    }

    #[test]
    fn arm_pc_relative_switch_load_seeds_all_guarded_table_entries() {
        let base = 0x1000u32;
        let mut data = vec![0u8; 0x60];
        // CMP r0,#3; three flag-preserving instructions; LDRCC pc,[pc,r0,LSL #2].
        for (off, word) in [
            (0x00, 0xE350_0003u32),
            (0x04, 0xE1A0_0000),
            (0x08, 0xE1A0_0000),
            (0x0C, 0xE1A0_0000),
            (0x10, 0x379F_F100),
            (0x18, base + 0x30),
            (0x1C, base + 0x41),
            (0x20, base + 0x50),
        ] {
            data[off..off + 4].copy_from_slice(&word.to_le_bytes());
        }
        assert_eq!(
            arm_pc_relative_jump_table_targets(&data, base, base + 0x10, 0x379F_F100),
            vec![
                (base + 0x30, false),
                (base + 0x40, true),
                (base + 0x50, false)
            ]
        );
    }

    #[test]
    fn arm_v6_saturation_instructions_decode_before_single_transfers() {
        assert_eq!(decode_arm(0xE6EF_9012, 0x1000), "usat r9, #15, r2;");
        assert_eq!(decode_arm(0xE6EF_8191, 0x1000), "usat r8, #15, r1, lsl #3;");
        assert_eq!(decode_arm(0xE6A6_9012, 0x1000), "ssat r9, #7, r2;");
        assert_eq!(decode_arm(0xE6E7_6F35, 0x1000), "usat16 r6, #7, r5;");
        assert_eq!(decode_arm(0xE6A6_7F34, 0x1000), "ssat16 r7, #7, r4;");
    }

    #[test]
    fn arm_load_writeback_snapshots_offset_before_overwriting_destination() {
        let stem = format!("dq8-ldr-writeback-rd-rm-{}", process::id());
        let input = std::env::temp_dir().join(format!("{stem}.bin"));
        let output = std::env::temp_dir().join(format!("{stem}.c"));
        // LDR r1, [r0, r1, LSL #2]!; BX lr. The loaded r1 must not
        // become the offset used to write r0 back.
        let code = [0xE7B0_1101u32, 0xE12F_FF1Eu32]
            .into_iter()
            .flat_map(u32::to_le_bytes)
            .collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        recompile_arm_function(&input, &output, 0x1000, 0x1000, &[]).unwrap();
        let generated = fs::read_to_string(&output).unwrap();
        assert!(generated.contains(
            "uint32_t _mem_base=r[0], _mem_delta=arm_shift_imm(r[1], 0, 2, c), _mem_adjusted=_mem_base + _mem_delta, _mem_ea=_mem_adjusted;"
        ));
        assert!(generated.contains("r[1] = mem32(_mem_ea); r[0] = _mem_adjusted;"));
        let _ = fs::remove_file(input);
        let _ = fs::remove_file(output);
    }

    #[test]
    fn d8_thumb_relative_pointer_initializer_table_is_resolved() {
        let base = 0x1000;
        let mut data = vec![0u8; 0x200];
        let insns = [
            0xB570u16, 0x4C06, 0x447C, 0x4D06, 0x447D, 0xE003, 0x6820, 0x1900, 0x4780, 0x1D24,
            0x42AC, 0xD1F9,
        ];
        for (i, h) in insns.into_iter().enumerate() {
            data[i * 2..i * 2 + 2].copy_from_slice(&h.to_le_bytes());
        }
        data[0x1C..0x20].copy_from_slice(&0x38u32.to_le_bytes());
        data[0x20..0x24].copy_from_slice(&0x3Cu32.to_le_bytes());
        data[0x40..0x44].copy_from_slice(&0x40i32.to_le_bytes());
        data[0x44..0x48].copy_from_slice(&0x41i32.to_le_bytes());
        let found = discover_thumb_relptr_init_table(&data, base, base + 0x200, base);
        assert_eq!(found.len(), 2);
        assert!(found.contains(&FuncKey {
            addr: 0x1080,
            mode: ArmMode::Arm
        }));
        assert!(found.contains(&FuncKey {
            addr: 0x1084,
            mode: ArmMode::Thumb
        }));
    }
    #[test]
    fn get_process_info_abi_survives_the_guest_arm_wrapper() {
        // Match the real wrapper: save the output pointer, issue SVC 2B,
        // then store r1/r2. A helper that writes memory instead of registers
        // will have its result overwritten by this wrapper.
        let dir = std::env::temp_dir().join(format!("dq8-process-info-abi-{}", process::id()));
        fs::create_dir_all(&dir).unwrap();
        let input = dir.join("stub.code");
        let output = dir.join("stub.c");
        let executable = dir.join("stub");
        let romfs = dir.join("empty.romfs");
        let words = [
            0xE3A0_0080u32, // MOV r0,#0x80 (output pointer)
            0xE59F_1024,    // LDR r1,=0xFFFF8001 (current process)
            0xE3A0_2014,    // MOV r2,#20 (linear-memory physical delta)
            0xE52D_0004,    // PUSH {r0}
            0xEF00_002B,    // SVC GetProcessInfo
            0xE49D_3004,    // POP {r3}
            0xE583_1000,    // STR r1,[r3]
            0xE583_2004,    // STR r2,[r3,#4]
            0xE593_0000,    // LDR r0,[r3]
            0xE593_2004,    // LDR r2,[r3,#4]
            0xE180_0002,    // ORR r0,r0,r2 (also check the high word is zero)
            0xE12F_FF1E,    // BX lr
            0xFFFF_8001,
        ];
        let code = words.into_iter().flat_map(u32::to_le_bytes).collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        let mut image = vec![0u8; 0x2000];
        image[..4].copy_from_slice(b"IVFC");
        image[0x3C..0x44].copy_from_slice(&0x1000u64.to_le_bytes());
        image[0x44..0x4C].copy_from_slice(&0x1000u64.to_le_bytes());
        image[0x4C..0x50].copy_from_slice(&12u32.to_le_bytes());
        image[0x54..0x58].copy_from_slice(&0x5Cu32.to_le_bytes());
        fs::write(&romfs, image).unwrap();
        recompile_arm_function(&input, &output, 0x0010_0000, 0x0010_0000, &[]).unwrap();
        let compiled = Command::new("clang")
            .args(["-std=c11", "-O0", "-w"])
            .arg(&output).arg("-o").arg(&executable).arg("-lm")
            .output().expect("clang is required for the native ABI regression test");
        assert!(compiled.status.success(), "{}", String::from_utf8_lossy(&compiled.stderr));
        let run = Command::new("timeout").arg("5s").arg(&executable)
            .arg(&input).arg(&romfs).env("DQ8_DISPLAY", "0")
            .env_remove("DQ8_PROGRESS_SECONDS").output().unwrap();
        assert!(run.status.success(), "{}", String::from_utf8_lossy(&run.stderr));
        assert!(String::from_utf8_lossy(&run.stdout).contains("r0=0x0C000000"),
                "{}", String::from_utf8_lossy(&run.stdout));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn arm_clz_preserves_flags_and_sizes_the_render_target() {
        // The game computes ceil_pow2(240) * 2 for the render-target width.
        // Exercise aliased/zero operands and NZCV preservation as well.
        let dir = std::env::temp_dir().join(format!("dq8-clz-native-{}", process::id()));
        fs::create_dir_all(&dir).unwrap();
        let input = dir.join("stub.code");
        let output = dir.join("stub.c");
        let executable = dir.join("stub");
        let romfs = dir.join("empty.romfs");
        let words = [
            0xE3A0_80EFu32, // MOV r8,#239
            0xE3A0_7102,    // MOV r7,#0x80000000
            0xE3A0_1002,    // MOV r1,#2
            0xE351_0002,    // CMP r1,#2 (Z=1,C=1)
            0xE16F_0F18,    // CLZ r0,r8 (24), flags unchanged
            0x13A0_0000,    // MOVNE r0,#0 (must not execute)
            0xE240_0001,    // SUB r0,r0,#1
            0xE1A0_0037,    // MOV r0,r7,LSR r0 (256)
            0xE005_0190,    // MUL r5,r0,r1 (512)
            0xE3A0_0000,    // MOV r0,#0
            0xE16F_0F10,    // CLZ r0,r0 (32)
            0xE240_0020,    // SUB r0,r0,#32
            0xE180_0005,    // ORR r0,r0,r5 (512)
            0xE12F_FF1E,    // BX lr
        ];
        let code = words.into_iter().flat_map(u32::to_le_bytes).collect::<Vec<_>>();
        fs::write(&input, code).unwrap();
        let mut image = vec![0u8; 0x2000];
        image[..4].copy_from_slice(b"IVFC");
        image[0x3C..0x44].copy_from_slice(&0x1000u64.to_le_bytes());
        image[0x44..0x4C].copy_from_slice(&0x1000u64.to_le_bytes());
        image[0x4C..0x50].copy_from_slice(&12u32.to_le_bytes());
        image[0x54..0x58].copy_from_slice(&0x5Cu32.to_le_bytes());
        fs::write(&romfs, image).unwrap();
        recompile_arm_function(&input, &output, 0x0010_0000, 0x0010_0000, &[]).unwrap();
        let compiled = Command::new("clang")
            .args(["-std=c11", "-O0", "-w"])
            .arg(&output).arg("-o").arg(&executable).arg("-lm")
            .output().expect("clang is required for the native CLZ regression test");
        assert!(compiled.status.success(), "{}", String::from_utf8_lossy(&compiled.stderr));
        let run = Command::new("timeout").arg("5s").arg(&executable)
            .arg(&input).arg(&romfs).env("DQ8_DISPLAY", "0")
            .env_remove("DQ8_PROGRESS_SECONDS").output().unwrap();
        assert!(run.status.success(), "{}", String::from_utf8_lossy(&run.stderr));
        assert!(String::from_utf8_lossy(&run.stdout).contains("r0=0x00000200"),
                "{}", String::from_utf8_lossy(&run.stdout));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn thread_wait_poll_and_main_join_use_the_kernel_abi() {
        // A live child must fail a zero-timeout poll, then an infinite join
        // must execute that child until ExitThread. r1 is deliberately junk.
        let dir = std::env::temp_dir().join(format!("dq8-thread-wait-native-{}", process::id()));
        fs::create_dir_all(&dir).unwrap();
        let input = dir.join("stub.code");
        let output = dir.join("stub.c");
        let executable = dir.join("stub");
        let romfs = dir.join("empty.romfs");
        // After joining, verify the child wrote 0xA5 before ExitThread.
        let words = [
            0xE1A0500Eu32,
            0xE3A08080u32,
            0xE3A0002Fu32,
            0xE59F1050u32,
            0xE3A02000u32,
            0xE3A03401u32,
            0xE3A04000u32,
            0xEF000008u32,
            0xE1A07001u32,
            0xE1A00007u32,
            0xE3E01000u32,
            0xE3A02000u32,
            0xE3A03000u32,
            0xEF000024u32,
            0xE1A06000u32,
            0xE1A00007u32,
            0xE3E02000u32,
            0xE1A03002u32,
            0xEF000024u32,
            0xE5981000u32,
            0xE22110A5u32,
            0xE1800001u32,
            0xE1800006u32,
            0xE1A0E005u32,
            0xE12FFF1Eu32,
            0x00100100u32,
        ];
        let mut code = words.into_iter().flat_map(u32::to_le_bytes).collect::<Vec<_>>();
        code.resize(0x100, 0);
        for word in [0xE3A00080u32, 0xE3A010A5u32, 0xE5801000u32, 0xEF000009u32, 0xE12FFF1Eu32] {
            code.extend_from_slice(&word.to_le_bytes());
        }
        fs::write(&input, code).unwrap();
        let mut image = vec![0u8; 0x2000];
        image[..4].copy_from_slice(b"IVFC");
        image[0x3C..0x44].copy_from_slice(&0x1000u64.to_le_bytes());
        image[0x44..0x4C].copy_from_slice(&0x1000u64.to_le_bytes());
        image[0x4C..0x50].copy_from_slice(&12u32.to_le_bytes());
        image[0x54..0x58].copy_from_slice(&0x5Cu32.to_le_bytes());
        fs::write(&romfs, image).unwrap();
        recompile_arm_function(&input, &output, 0x0010_0000, 0x0010_0000, &[0x0010_0100]).unwrap();
        let compiled = Command::new("clang")
            .args(["-std=c11", "-O0", "-w"])
            .arg(&output).arg("-o").arg(&executable).arg("-lm")
            .output().expect("clang is required for the native thread-wait regression test");
        assert!(compiled.status.success(), "{}", String::from_utf8_lossy(&compiled.stderr));
        let run = Command::new("timeout").arg("5s").arg(&executable)
            .arg(&input).arg(&romfs).env("DQ8_DISPLAY", "0")
            .env("DQ8_RUN_GUEST_THREADS", "1").env_remove("DQ8_PROGRESS_SECONDS").output().unwrap();
        assert!(run.status.success(), "{}", String::from_utf8_lossy(&run.stderr));
        assert!(String::from_utf8_lossy(&run.stdout).contains("r0=0x09401BFE"),
                "{}", String::from_utf8_lossy(&run.stdout));
        fs::remove_dir_all(dir).unwrap();
    }


}
