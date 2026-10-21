//! Minimal ZIP writer shared by Skill bundles and diagnostic exports.
//!
//! The target image does not guarantee a ZIP library, so entries are stored
//! without compression. This keeps the implementation small and deterministic;
//! callers remain responsible for their own content-size limits.

pub(crate) struct StoredZip {
    data: Vec<u8>,
    central: Vec<u8>,
    entries: u16,
}

impl StoredZip {
    pub(crate) fn new() -> Self {
        Self {
            data: Vec::new(),
            central: Vec::new(),
            entries: 0,
        }
    }

    pub(crate) fn add(&mut self, name: &str, content: &[u8]) -> Result<(), String> {
        if self.entries == u16::MAX {
            return Err("ZIP 条目数量超出限制".to_string());
        }
        if name.is_empty() || name.len() > u16::MAX as usize {
            return Err("ZIP 文件路径长度超出限制".to_string());
        }
        if name.starts_with('/')
            || name.contains('\\')
            || name.split('/').any(|part| part == ".." || part.is_empty())
        {
            return Err(format!("ZIP 文件路径不合法: {}", name));
        }
        if content.len() > u32::MAX as usize || self.data.len() > u32::MAX as usize {
            return Err("ZIP 体积超出格式限制".to_string());
        }

        let name = name.as_bytes();
        let size = content.len() as u32;
        let crc = crc32(content);
        let offset = self.data.len() as u32;
        let flags = 0x0800u16;

        push_u32(&mut self.data, 0x0403_4b50);
        push_u16(&mut self.data, 20);
        push_u16(&mut self.data, flags);
        push_u16(&mut self.data, 0);
        push_u16(&mut self.data, 0);
        push_u16(&mut self.data, 0x0021);
        push_u32(&mut self.data, crc);
        push_u32(&mut self.data, size);
        push_u32(&mut self.data, size);
        push_u16(&mut self.data, name.len() as u16);
        push_u16(&mut self.data, 0);
        self.data.extend_from_slice(name);
        self.data.extend_from_slice(content);

        push_u32(&mut self.central, 0x0201_4b50);
        push_u16(&mut self.central, 20);
        push_u16(&mut self.central, 20);
        push_u16(&mut self.central, flags);
        push_u16(&mut self.central, 0);
        push_u16(&mut self.central, 0);
        push_u16(&mut self.central, 0x0021);
        push_u32(&mut self.central, crc);
        push_u32(&mut self.central, size);
        push_u32(&mut self.central, size);
        push_u16(&mut self.central, name.len() as u16);
        push_u16(&mut self.central, 0);
        push_u16(&mut self.central, 0);
        push_u16(&mut self.central, 0);
        push_u16(&mut self.central, 0);
        push_u32(&mut self.central, 0);
        push_u32(&mut self.central, offset);
        self.central.extend_from_slice(name);
        self.entries += 1;
        Ok(())
    }

    pub(crate) fn finish(mut self) -> Result<Vec<u8>, String> {
        if self.data.len() > u32::MAX as usize || self.central.len() > u32::MAX as usize {
            return Err("ZIP 体积超出格式限制".to_string());
        }
        let central_offset = self.data.len() as u32;
        let central_size = self.central.len() as u32;
        self.data.extend_from_slice(&self.central);
        push_u32(&mut self.data, 0x0605_4b50);
        push_u16(&mut self.data, 0);
        push_u16(&mut self.data, 0);
        push_u16(&mut self.data, self.entries);
        push_u16(&mut self.data, self.entries);
        push_u32(&mut self.data, central_size);
        push_u32(&mut self.data, central_offset);
        push_u16(&mut self.data, 0);
        Ok(self.data)
    }
}

fn push_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn crc32(content: &[u8]) -> u32 {
    let mut crc = u32::MAX;
    for byte in content {
        crc ^= *byte as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xedb8_8320 & mask);
        }
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writer_rejects_escaping_paths() {
        let mut zip = StoredZip::new();
        assert!(zip.add("../secret", b"x").is_err());
        assert!(zip.add("/absolute", b"x").is_err());
        assert!(zip.add("a\\b", b"x").is_err());
    }

    #[test]
    fn empty_payload_has_a_stable_crc() {
        assert_eq!(crc32(b""), 0);
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
    }
}
