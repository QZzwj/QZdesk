//! Local, file-backed Skill retrieval for the MCP gateway.
//!
//! A Skill is a directory containing SKILL.md plus optional Markdown
//! references.  The directory is scanned on every request, so installing or
//! removing a Skill takes effect without rebuilding the Rust binary.

use async_trait::async_trait;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::SystemTime;
use tokio::process::Command;

use super::tool::McpTool;

const MAX_FILE_BYTES: u64 = 1024 * 1024;
const MAX_READ_CHARS: usize = 16_000;
const MAX_SEARCH_RESULTS: usize = 6;
const MAX_SNIPPET_CHARS: usize = 2_400;
const MAX_INSTRUCTIONS_CHARS: usize = 24_000;
/// 同一个 Skill 最多返回几条：结果列表要说清「哪个技能、哪一节」，
/// 不能被单个技能的相邻章节占满。
const MAX_HITS_PER_SKILL: usize = 2;

/// 内容摘要：SHA-256 前 16 位十六进制。够区分内容，日志里也读得下去。
fn digest(input: &str) -> String {
    let mut hasher = Sha256::new();
    hasher.update(input.as_bytes());
    hasher
        .finalize()
        .iter()
        .take(8)
        .map(|byte| format!("{:02x}", byte))
        .collect()
}

fn configured_skill_root() -> Option<String> {
    ["QZDESK_SKILL_DIR", "XIAOZHI_SKILL_DIR"]
        .into_iter()
        .find_map(|name| env::var(name).ok().filter(|value| !value.trim().is_empty()))
}

/// Optional read-only source tree for Skills checked out next to QZdesk.
/// This is separate from the writable install root so the web manager never
/// edits a source repository when a user edits or removes an imported Skill.
fn configured_skill_source_root() -> Option<String> {
    ["QZDESK_SKILL_SOURCE_DIR", "XIAOZHI_SKILL_SOURCE_DIR"]
        .into_iter()
        .find_map(|name| env::var(name).ok().filter(|value| !value.trim().is_empty()))
}

/// Pick a writable persistent location. `/userdata` is used on the target
/// device, while the simulator/developer build falls back to the user's data
/// directory instead of trying to create a root-level directory.
fn default_user_skill_root() -> PathBuf {
    let device_root = PathBuf::from("/userdata/xiaozhi/skills");
    if fs::create_dir_all(&device_root).is_ok() {
        return device_root;
    }
    let base = env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .or_else(|| env::var_os("HOME").map(|home| PathBuf::from(home).join(".local/share")))
        .unwrap_or_else(|| PathBuf::from("."));
    base.join("qzdesk/skills")
}

/// A Skill is always retained in the local pool after installation.  The
/// selection below only controls whether the cloud agent may use it, and how
/// it is loaded into a conversation.
#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, Default)]
#[serde(rename_all = "lowercase")]
pub enum SkillRole {
    #[default]
    None,
    Primary,
    Secondary,
}

impl SkillRole {
    fn as_str(&self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Primary => "primary",
            Self::Secondary => "secondary",
        }
    }

    pub(crate) fn parse(value: &str) -> Result<Self, String> {
        match value {
            "none" => Ok(Self::None),
            "primary" => Ok(Self::Primary),
            "secondary" => Ok(Self::Secondary),
            _ => Err("Skill 类型只能是 none、primary 或 secondary".to_string()),
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct SkillSelection {
    /// 只保留 role：是否启用由 role != none 推导，避免两个字段互相矛盾
    #[serde(default)]
    role: SkillRole,
}

impl SkillSelection {
    fn is_active(&self) -> bool {
        self.role != SkillRole::None
    }
}

impl Default for SkillSelection {
    fn default() -> Self {
        /* 磁盘上扫到、但用户从未选过的 Skill（OEM / 预置）默认关闭；
         * 网页新导入的会被显式设为主技能，见 `activate_new_skill`。 */
        Self { role: SkillRole::None }
    }
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
struct SkillState {
    #[serde(default)]
    skills: BTreeMap<String, SkillSelection>,
}

/// 一个按标题切分出来的 Markdown 章节，检索以章节为最小单位
#[derive(Clone)]
struct IndexedChunk {
    title: String,
    /// 1 起始的行号，供 skill_read 精确续读
    line_start: usize,
    text: String,
    lower: String,
}

/// 一个已建立检索索引的 Markdown 文件
#[derive(Clone)]
struct IndexedFile {
    relative: String,
    size: u64,
    mtime: Option<SystemTime>,
    chunks: Vec<IndexedChunk>,
}

#[derive(Clone, Default)]
struct IndexedSkill {
    files: Vec<IndexedFile>,
}

/// Skill 索引缓存：文件指纹未变化时直接复用，避免每次检索都全量读盘
#[derive(Default)]
struct SkillIndex {
    skills: BTreeMap<String, IndexedSkill>,
}

#[derive(Clone)]
pub struct SkillManager {
    roots: Vec<PathBuf>,
    /// 角色选择的落盘位置。`None` = 按安装根目录推（生产路径，可被
    /// `XIAOZHI_SKILL_STATE_FILE` 覆盖）；测试里显式给一个临时文件，
    /// 免得测试把用户真实的 `skill_state.json` 覆盖掉。
    state_file: Option<PathBuf>,
    index: Arc<Mutex<SkillIndex>>,
}

impl SkillManager {
    pub fn new() -> Self {
        let mut roots = Vec::new();
        if let Some(path) = configured_skill_root() {
            roots.push(PathBuf::from(path));
        }
        if let Some(path) = configured_skill_source_root() {
            roots.push(PathBuf::from(path));
        }
        // Production and persistent locations.
        roots.push(PathBuf::from("/userdata/xiaozhi/skills"));
        roots.push(PathBuf::from("/oem/usr/share/xiaozhi/skills"));
        // Persistent user storage is also used by the simulator and local
        // development. Project checkouts are intentionally not scanned by
        // default, so vendored reference Skills cannot appear in the manager.
        roots.push(default_user_skill_root());
        // Preserve precedence: an explicitly configured directory wins over
        // the OEM/userdata/default locations when Skill names collide.
        let mut unique_roots = Vec::with_capacity(roots.len());
        for root in roots {
            if !unique_roots.iter().any(|existing| existing == &root) {
                unique_roots.push(root);
            }
        }
        Self {
            roots: unique_roots,
            state_file: None,
            index: Arc::new(Mutex::new(SkillIndex::default())),
        }
    }

    /// 只用于测试：技能目录与状态文件都落在给定的临时目录里。
    ///
    /// 不去改进程级环境变量（并行跑起来不稳），也不会碰到用户真实的
    /// `skill_state.json` —— 测试改角色曾经真把开发机上的选择写坏过。
    #[cfg(test)]
    pub(crate) fn for_test_dir(dir: PathBuf) -> Self {
        Self {
            roots: vec![dir.clone()],
            state_file: Some(dir.join("skill_state.json")),
            index: Arc::new(Mutex::new(SkillIndex::default())),
        }
    }

    /// 让索引缓存整体失效（安装/删除 Skill 后调用）
    pub(crate) fn invalidate_index(&self) {
        if let Ok(mut index) = self.index.lock() {
            index.skills.clear();
        }
    }

    fn skill_dirs(&self) -> Vec<(String, PathBuf)> {
        let mut result = Vec::new();
        for root in &self.roots {
            // Git repositories commonly keep the actual collection under a
            // second `skills/` directory (for example skills/skills/animate).
            // Treat both layouts identically while preserving root precedence.
            if root.join("SKILL.md").is_file() {
                let name = root
                    .file_name()
                    .and_then(|value| value.to_str())
                    .unwrap_or_default()
                    .to_string();
                if !name.is_empty() && !result.iter().any(|(candidate, _)| candidate == &name) {
                    result.push((name, root.clone()));
                }
                continue;
            }
            let nested = root.join("skills");
            let scan_roots = if nested.is_dir() {
                vec![root.clone(), nested]
            } else {
                vec![root.clone()]
            };
            for scan_root in scan_roots {
                let entries = match fs::read_dir(&scan_root) {
                    Ok(entries) => entries,
                    Err(_) => continue,
                };
                for entry in entries.flatten() {
                    let path = entry.path();
                    match fs::symlink_metadata(&path) {
                        Ok(meta) if meta.file_type().is_dir() => {}
                        _ => continue,
                    }
                    match fs::symlink_metadata(path.join("SKILL.md")) {
                        Ok(meta) if meta.file_type().is_file() => {}
                        _ => continue,
                    }
                    let name = path
                        .file_name()
                        .and_then(|v| v.to_str())
                        .unwrap_or_default()
                        .to_string();
                    if !name.is_empty() && !result.iter().any(|(n, _)| n == &name) {
                        result.push((name, path));
                    }
                }
            }
        }
        result.sort_by(|a, b| a.0.cmp(&b.0));
        result
    }

    fn find_skill(&self, name: &str) -> Option<PathBuf> {
        self.skill_dirs()
            .into_iter()
            .find(|(candidate, _)| candidate == name)
            .map(|(_, path)| path)
    }

    fn state_path(&self) -> PathBuf {
        if let Some(path) = &self.state_file {
            return path.clone();
        }
        if let Ok(path) = env::var("XIAOZHI_SKILL_STATE_FILE") {
            return PathBuf::from(path);
        }
        self.install_root()
            .parent()
            .unwrap_or_else(|| Path::new("."))
            .join("skill_state.json")
    }

    fn load_state(&self) -> SkillState {
        let path = self.state_path();
        match fs::read_to_string(&path) {
            Ok(content) => match serde_json::from_str(&content) {
                Ok(state) => state,
                Err(error) => {
                    log::warn!("Ignoring invalid Skill state {}: {}", path.display(), error);
                    SkillState::default()
                }
            },
            Err(_) => SkillState::default(),
        }
    }

    fn save_state(&self, state: &SkillState) -> Result<(), String> {
        let path = self.state_path();
        let parent = path.parent().unwrap_or_else(|| Path::new("."));
        fs::create_dir_all(parent).map_err(|e| format!("无法创建 Skill 状态目录: {}", e))?;
        let content = serde_json::to_vec_pretty(state)
            .map_err(|e| format!("无法编码 Skill 状态: {}", e))?;
        let temporary = path.with_extension("json.tmp");
        fs::write(&temporary, content).map_err(|e| format!("无法保存 Skill 状态: {}", e))?;
        fs::rename(&temporary, &path).map_err(|e| format!("无法提交 Skill 状态: {}", e))
    }

    fn selection<'a>(state: &'a SkillState, name: &str) -> SkillSelection {
        state.skills.get(name).cloned().unwrap_or_default()
    }

    fn active_skill_dirs(&self) -> Vec<(String, PathBuf)> {
        let state = self.load_state();
        self.skill_dirs()
            .into_iter()
            .filter(|(name, _)| Self::selection(&state, name).is_active())
            .collect()
    }

    ////////////////////// 检索索引 //////////////////////

    /// 把一份 Markdown 按标题切成章节；无标题时退化为整篇一个章节
    fn chunk_markdown(content: &str) -> Vec<IndexedChunk> {
        let mut chunks: Vec<IndexedChunk> = Vec::new();
        let mut title = String::from("概述");
        let mut text = String::new();
        let mut line_start = 1usize;

        for (idx, line) in content.lines().enumerate() {
            let line_no = idx + 1;
            let trimmed = line.trim_start();
            if trimmed.starts_with('#') {
                if !text.trim().is_empty() {
                    chunks.push(Self::finish_chunk(&title, line_start, &text));
                    text.clear();
                }
                let heading = trimmed.trim_start_matches('#').trim();
                title = if heading.is_empty() { String::from("概述") } else { heading.to_string() };
                line_start = line_no;
                continue;
            }
            if text.trim().is_empty() && line.trim().is_empty() {
                line_start = line_no + 1;
            }
            text.push_str(line);
            text.push('\n');
        }

        if !text.trim().is_empty() {
            chunks.push(Self::finish_chunk(&title, line_start, &text));
        }
        chunks
    }

    fn finish_chunk(title: &str, line_start: usize, text: &str) -> IndexedChunk {
        let text = text.trim_end().to_string();
        IndexedChunk {
            title: title.to_string(),
            line_start,
            lower: text.to_lowercase(),
            text,
        }
    }

    /// 读取（或复用缓存的）某个 Skill 的检索索引
    fn indexed_skill(&self, name: &str, dir: &Path) -> IndexedSkill {
        let mut paths = Vec::new();
        collect_markdown_files(dir, &mut paths);
        paths.sort();

        let mut current: Vec<(String, PathBuf, u64, Option<SystemTime>)> = Vec::new();
        for path in paths {
            let metadata = match fs::metadata(&path) {
                Ok(metadata) if metadata.len() <= MAX_FILE_BYTES => metadata,
                _ => continue,
            };
            let relative = path
                .strip_prefix(dir)
                .unwrap_or(&path)
                .to_string_lossy()
                .trim_start_matches('/')
                .to_string();
            current.push((relative, path, metadata.len(), metadata.modified().ok()));
        }

        // 指纹一致就直接复用缓存
        if let Ok(index) = self.index.lock() {
            if let Some(cached) = index.skills.get(name) {
                let same = cached.files.len() == current.len()
                    && cached
                        .files
                        .iter()
                        .zip(current.iter())
                        .all(|(a, b)| a.relative == b.0 && a.size == b.2 && a.mtime == b.3);
                if same {
                    return cached.clone();
                }
            }
        }

        let mut skill = IndexedSkill::default();
        for (relative, path, size, mtime) in current {
            let content = match fs::read_to_string(&path) {
                Ok(content) => content,
                Err(_) => continue,
            };
            skill.files.push(IndexedFile {
                relative,
                size,
                mtime,
                chunks: Self::chunk_markdown(&content),
            });
        }

        if let Ok(mut index) = self.index.lock() {
            index.skills.insert(name.to_string(), skill.clone());
        }
        skill
    }

    /// 拆分检索词：ASCII 按空白/标点切分，中文额外补充字符二元组
    fn query_terms(query_lower: &str) -> Vec<String> {
        let mut terms: Vec<String> = Vec::new();

        for token in query_lower.split(|c: char| {
            c.is_whitespace() || matches!(c, ',' | '，' | '。' | '、' | '?' | '？' | '!' | '！' | ':' | '：' | ';' | '；')
        }) {
            let token = token.trim();
            if !token.is_empty() {
                terms.push(token.to_string());
            }
        }

        // 中文没有空格，整句子串匹配召回很差，这里补字符二元组与三元组：
        // 二元组保召回（"天气" 能命中 "天气预报"），三元组保精度（"天气预报"
        // 不会被泛词 "天气" 淹没）。没有分词器的前提下，这两层最划算。
        let chars: Vec<char> = query_lower
            .chars()
            .filter(|c| !c.is_whitespace() && !c.is_ascii_punctuation())
            .collect();
        for width in [2_usize, 3_usize] {
            for window in chars.windows(width) {
                if window.iter().all(|c| c.is_ascii()) {
                    continue;
                }
                terms.push(window.iter().collect());
            }
        }

        terms.sort();
        terms.dedup();
        terms
    }

    /// 一个检索词的分量：中文三元组 > 中文二元组 ≈ ASCII 单词。
    ///
    /// 词越长越具体，命中它越能说明「就是这一段」；二元组只用来兜召回，
    /// 所以权重给低一档。
    fn term_weight(term: &str) -> u32 {
        if term.is_ascii() {
            6
        } else if term.chars().count() >= 3 {
            12
        } else {
            7
        }
    }

    /// 片段打分：整句命中 > 标题命中 > 正文词频，另有技能名带来的整体加成。
    ///
    /// 返回 `(分数, 命中词)`。命中词一起带出去：网页和模型都能看清「为什么是
    /// 这一条」，检索没召回时也便于判断是词切得不对还是内容确实没有。
    fn score_chunk(
        query_lower: &str,
        terms: &[String],
        chunk: &IndexedChunk,
        names: &[String],
    ) -> (u32, Vec<String>) {
        let mut score = 0u32;
        let mut matched: Vec<String> = Vec::new();

        // 整句命中给最高权重
        if !query_lower.is_empty() && chunk.lower.contains(query_lower) {
            score += 60;
        }

        // 片段正文里直接写到技能名：这条基本就是答案本身
        if names
            .iter()
            .any(|name| !name.is_empty() && chunk.lower.contains(name.as_str()))
        {
            score += 20;
        }

        let title_lower = chunk.title.to_lowercase();
        for term in terms {
            let hits = chunk.lower.matches(term.as_str()).count() as u32;
            if hits == 0 {
                // 正文没有、技能名里有：算弱相关，别整条丢掉
                if names.iter().any(|name| name.contains(term.as_str())) {
                    matched.push(term.clone());
                    score += 3;
                }
                continue;
            }
            matched.push(term.clone());
            score += Self::term_weight(term) + hits.min(3) * 3;
            if !title_lower.is_empty() && title_lower.contains(term.as_str()) {
                // 标题权重：章节标题往往就是答案所在
                score += 10;
            }
        }

        // 问题里直接点名这个技能（"HQQ 怎么写"）：整体提到前面
        if names
            .iter()
            .any(|name| !name.is_empty() && query_lower.contains(name.as_str()))
        {
            score += 25;
        }

        (score, matched)
    }

    /// 选择片段锚点：优先整句命中，其次第一个命中的检索词
    fn first_hit(lower: &str, query_lower: &str, terms: &[String]) -> usize {
        if let Some(position) = lower.find(query_lower) {
            return position;
        }
        terms
            .iter()
            .filter_map(|term| lower.find(term.as_str()))
            .min()
            .unwrap_or(0)
    }

    /// 给网页 / 设备列表用：包含已关闭的技能（用户还要能重新打开它们）。
    pub(crate) fn list(&self) -> Value {
        self.list_inner(true)
    }

    /// 这个 Skill 是不是装在用户目录里的（可编辑、可删除）。
    ///
    /// OEM / 源码目录里的同名技能也能读，但保存会写到用户目录形成覆盖版本；
    /// 界面据此提示，`skill_status` 也把这一项报出来。
    pub(crate) fn is_user_installed(&self, name: &str) -> bool {
        self.install_root().join(name).join("SKILL.md").is_file()
    }

    /// 给云端模型用：只列本会话可用的技能。
    ///
    /// 已关闭的技能不出现在这里 —— 模型看得见却读不到（`skill_read` 会拒绝一个
    /// 非生效技能）只会浪费一轮工具调用，也模糊了"关闭"的含义。
    pub(crate) fn list_for_model(&self) -> Value {
        self.list_inner(false)
    }

    fn list_inner(&self, include_disabled: bool) -> Value {
        let state = self.load_state();
        let mut entries: Vec<(u8, String, Value)> = self
            .skill_dirs()
            .into_iter()
            .map(|(dir_name, path)| {
                let metadata = parse_skill_metadata(&path.join("SKILL.md"));
                let selection = Self::selection(&state, &dir_name);
                let rank = match selection.role {
                    SkillRole::Primary => 0u8,
                    SkillRole::Secondary => 1,
                    SkillRole::None => 2,
                };
                let value = json!({
                    "id": dir_name.clone(),
                    "name": metadata.name.unwrap_or_else(|| dir_name.clone()),
                    "description": metadata.description.unwrap_or_default(),
                    "path": path.display().to_string(),
                    "enabled": selection.is_active(),
                    "active": selection.is_active(),
                    "role": selection.role.as_str(),
                    // 只读（OEM / 源码目录）的技能也能编辑，但保存会写成用户覆盖版本，
                    // 界面据此提前说明，别让用户以为改的是原目录
                    "writable": self.is_user_installed(&dir_name),
                });
                (rank, dir_name, value)
            })
            .filter(|(_, _, value)| {
                include_disabled
                    || value.get("role").and_then(Value::as_str) != Some("none")
            })
            .collect();

        // 生效中的 Skill 排前面，方便网页一眼看清当前配置
        entries.sort_by(|a, b| a.0.cmp(&b.0).then_with(|| a.1.cmp(&b.1)));

        let (primary, secondary) = self.active_summary();
        let skills: Vec<Value> = entries.into_iter().map(|(_, _, value)| value).collect();
        if !include_disabled {
            // 模型侧的目录：不带网页上传限制这类与对话无关的信息
            return json!({
                "skills": skills,
                "count": skills.len(),
                "summary": { "primary": primary, "secondary": secondary }
            });
        }
        // 把当前生效的上限一并返回，网页据此显示（SD 卡启动时不限制解压体积/文件数）
        let limits = json!({
            "sdCard": booted_from_sd_card(),
            "unpackedBytes": unpacked_limits().map(|(bytes, _)| bytes),
            "maxFiles": unpacked_limits().map(|(_, files)| files),
            "zipBytes": MAX_SKILL_ZIP_BYTES
        });
        json!({
            "skills": skills,
            "count": skills.len(),
            "summary": {
                "primary": primary,
                "secondary": secondary,
                // 当前配置版本：网页显示它，会话侧比较它决定要不要重建
                "version": self.version(),
            },
            "limits": limits
        })
    }

    /// Directory used by the web manager for persistent user-installed Skills.
    /// OEM files remain available as read-only defaults; user files take
    /// precedence because `/userdata` is writable across firmware updates.
    pub(crate) fn install_root(&self) -> PathBuf {
        if let Some(path) = configured_skill_root() {
            return PathBuf::from(path);
        }
        default_user_skill_root()
    }

    pub(crate) fn install(&self, name: &str, skill_md: &str) -> Result<(), String> {
        validate_skill_name(name)?;
        if skill_md.trim().is_empty() {
            return Err("SKILL.md 内容不能为空".to_string());
        }
        if skill_md.len() as u64 > MAX_FILE_BYTES {
            return Err("SKILL.md 超过 1 MiB 限制".to_string());
        }
        // 覆盖已有的技能不算“导入”，角色交给用户决定
        let existed_before = self.find_skill(name).is_some();
        let root = self.install_root();
        fs::create_dir_all(&root).map_err(|e| format!("无法创建 Skill 目录: {}", e))?;
        let dir = root.join(name);
        fs::create_dir_all(&dir).map_err(|e| format!("无法创建 Skill: {}", e))?;
        let tmp = dir.join("SKILL.md.tmp");
        let mut file = fs::File::create(&tmp).map_err(|e| format!("无法写入 SKILL.md: {}", e))?;
        file.write_all(skill_md.as_bytes()).map_err(|e| format!("无法写入 SKILL.md: {}", e))?;
        file.sync_all().map_err(|e| format!("无法保存 SKILL.md: {}", e))?;
        fs::rename(&tmp, dir.join("SKILL.md"))
            .map_err(|e| format!("无法提交 SKILL.md: {}", e))?;
        sync_directory(&dir).map_err(|e| format!("无法持久化 Skill 目录: {}", e))?;
        sync_directory(&root).map_err(|e| format!("无法持久化 Skill 根目录: {}", e))?;
        // 导入即生效：这一句保存完，下一句对话就该按它回答
        self.activate_new_skill(name, existed_before);
        Ok(())
    }

    pub(crate) fn remove(&self, name: &str) -> Result<(), String> {
        validate_skill_name(name)?;
        let root = self.install_root();
        let dir = root.join(name);
        if !dir.exists() {
            return Err(format!("Skill '{}' 不存在或不是用户安装的 Skill", name));
        }
        fs::remove_dir_all(&dir).map_err(|e| format!("删除 Skill 失败: {}", e))?;
        let mut state = self.load_state();
        state.skills.remove(name);
        self.save_state(&state)
    }

    /// 设置某个 Skill 的使用方式。
    ///
    /// 允许同时存在多个主技能（例如“人设 + 语言风格”），不再像以前那样把旧主技能
    /// 隐式降级为备用技能——那种行为对使用者不可预期。
    pub(crate) fn set_role(&self, name: &str, role: SkillRole) -> Result<(), String> {
        validate_skill_name(name)?;
        if self.find_skill(name).is_none() {
            return Err(format!("Skill '{}' 不存在", name));
        }
        let mut state = self.load_state();
        state.skills.insert(name.to_string(), SkillSelection { role });
        self.save_state(&state)
    }

    /// 把一个 Skill 设成**唯一**主技能（原来的主技能降为备用）。
    ///
    /// 语音说「换成中医模式」走这里：一次写盘生效，不会留下两个主技能同时注入。
    /// 与网页控制台逐条改的选择是同一份状态，所以改完网页上也看得到。
    pub(crate) fn switch_primary(&self, name: &str) -> Result<(), String> {
        validate_skill_name(name)?;
        let discovered: Vec<String> = self
            .skill_dirs()
            .into_iter()
            .map(|(dir_name, _)| dir_name)
            .collect();
        if !discovered.iter().any(|dir_name| dir_name == name) {
            return Err(format!("Skill '{}' 不存在", name));
        }
        let mut state = self.load_state();
        for (dir_name, role) in primary_switch_plan(&state, &discovered, name) {
            state.skills.insert(dir_name, SkillSelection { role });
        }
        self.save_state(&state)
    }

    /// 网页新导入的 Skill 直接进主技能：导入完第一句对话就该按它回答。
    ///
    /// 判定规则见 `role_for_newly_installed`——覆盖编辑、以及用户已选过的角色
    /// 都不动，只给本次新出现的技能补主技能。状态写入失败不影响本次安装：
    /// 技能已经在池子里，用户仍可在列表里手动选。
    fn activate_new_skill(&self, name: &str, existed_before: bool) {
        let mut state = self.load_state();
        let Some(role) = role_for_newly_installed(&state, name, existed_before) else {
            return;
        };
        state.skills.insert(name.to_string(), SkillSelection { role });
        match self.save_state(&state) {
            Ok(()) => log::info!("新导入的 Skill '{}' 已默认设为主技能", name),
            Err(error) => log::warn!(
                "新导入的 Skill '{}' 未能默认设为主技能（可稍后在列表里手动设置）: {}",
                name,
                error
            ),
        }
    }

    /// 统计当前生效的主技能 / 备用技能数量，供网页显示
    pub(crate) fn active_summary(&self) -> (usize, usize) {
        let state = self.load_state();
        let mut primary = 0usize;
        let mut secondary = 0usize;
        for (name, _) in self.skill_dirs() {
            match Self::selection(&state, &name).role {
                SkillRole::Primary => primary += 1,
                SkillRole::Secondary => secondary += 1,
                SkillRole::None => {}
            }
        }
        (primary, secondary)
    }

    /// 读取某个 Skill 的 SKILL.md 原文（供网页查看/编辑，不要求已启用）
    pub(crate) fn read_source(&self, name: &str) -> Result<String, String> {
        validate_skill_name(name)?;
        let dir = self
            .find_skill(name)
            .ok_or_else(|| format!("Skill '{}' 不存在", name))?;
        let path = dir.join("SKILL.md");
        let metadata = fs::metadata(&path).map_err(|e| format!("无法读取 SKILL.md: {}", e))?;
        if metadata.len() > MAX_FILE_BYTES {
            return Err("SKILL.md 超过 1 MiB 限制".to_string());
        }
        fs::read_to_string(&path).map_err(|e| format!("SKILL.md 不是有效的 UTF-8 文件: {}", e))
    }

    pub(crate) async fn install_zip(&self, name: &str, archive_data: &[u8]) -> Result<(), String> {
        validate_skill_name(name)?;
        // 覆盖安装不算“导入”，别把用户选过的角色改掉
        let existed_before = self.find_skill(name).is_some();
        if archive_data.is_empty() || archive_data.len() > MAX_SKILL_ZIP_BYTES {
            return Err(format!(
                "ZIP 文件为空或超过 {} MiB 限制",
                MAX_SKILL_ZIP_BYTES / 1024 / 1024
            ));
        }
        // SD 卡启动时允许大 Skill，而 /tmp 只有 90MB tmpfs 装不下，
        // 把解压临时目录放到 Skill 根目录旁边：同一个数据分区，也不会被枚举成 Skill
        let temp = if booted_from_sd_card() {
            self.install_root()
                .parent()
                .map(Path::to_path_buf)
                .unwrap_or_else(std::env::temp_dir)
                .join(format!(".upload-{}-{}", name, std::process::id()))
        } else {
            std::env::temp_dir().join(format!("xiaozhi-upload-{}-{}", name, std::process::id()))
        };
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).map_err(|e| format!("无法创建临时目录: {}", e))?;
        let archive = temp.join("skill.zip");
        fs::write(&archive, archive_data).map_err(|e| format!("无法保存 ZIP: {}", e))?;
        // 解压前先估算体积，防止 zip 炸弹写满分分区
        if let Err(error) = check_archive_unpacked_size(&archive).await {
            let _ = fs::remove_dir_all(&temp);
            return Err(error);
        }
        // 解压到子目录：这样压缩包本体不会被当成技能内容一起复制进去（否则体积翻倍）
        let extract_dir = temp.join("extract");
        fs::create_dir_all(&extract_dir).map_err(|e| format!("无法创建临时解压目录: {}", e))?;
        let extract = Command::new("unzip")
            .args(["-q", archive.to_str().unwrap_or("skill.zip"), "-d", extract_dir.to_str().unwrap_or("extract")])
            .status().await.map_err(|e| format!("启动 unzip 失败: {}", e))?;
        if !extract.success() {
            let _ = fs::remove_dir_all(&temp);
            return Err("ZIP 解压失败，请确认文件完整".to_string());
        }
        // 解压后再按实际落盘体积复核一次，双保险（SD 卡启动时不限制）；
        // 只统计解压目录，不含压缩包本体
        let (unpacked_bytes, unpacked_files) = dir_stats(&extract_dir);
        if let Some((max_bytes, max_files)) = unpacked_limits() {
            if unpacked_bytes > max_bytes || unpacked_files > max_files {
                let _ = fs::remove_dir_all(&temp);
                return Err(format!(
                    "Skill 解压后过大（约 {} KB / {} 个文件），上限 {} MB / {} 个文件",
                    unpacked_bytes / 1024,
                    unpacked_files,
                    max_bytes / 1024 / 1024,
                    max_files
                ));
            }
        }
        let source = find_skill_root(&extract_dir).ok_or_else(|| "ZIP 中没有找到 SKILL.md".to_string())?;
        validate_skill_file(&source.join("SKILL.md"))?;
        let root = self.install_root();
        fs::create_dir_all(&root).map_err(|e| format!("无法创建 Skill 目录: {}", e))?;
        let target = root.join(name);
        let staging = root.join(format!(".{}.installing", name));
        let _ = fs::remove_dir_all(&staging);
        copy_dir(&source, &staging).map_err(|e| format!("复制 Skill 文件失败: {}", e))?;
        validate_skill_file(&staging.join("SKILL.md"))?;
        let _ = fs::remove_dir_all(&target);
        fs::rename(&staging, &target).map_err(|e| format!("安装 Skill 失败: {}", e))?;
        sync_directory(&target).map_err(|e| format!("无法持久化 Skill 目录: {}", e))?;
        sync_directory(&root).map_err(|e| format!("无法持久化 Skill 根目录: {}", e))?;
        let _ = fs::remove_dir_all(&temp);
        log::info!("Skill '{}' installed at {}", name, target.display());
        // 导入即生效：ZIP 装完，下一句对话就该按它回答
        self.activate_new_skill(name, existed_before);
        Ok(())
    }

    /// 当前 Skill 配置的版本号（内容摘要，SHA-256 前 16 位）。
    ///
    /// 四类输入一起算，正是「改了却没生效」会踩到的四件事：
    /// - 主技能 / 备用技能的 `SKILL.md` 正文（改一个字就该重开会话）
    /// - 技能的清单：名字与说明
    /// - 角色配置：主 / 备用 / 关闭
    /// - 每个 `SKILL.md` 的修改时间与大小
    ///
    /// 会话侧只要比较这个版本就知道要不要重建，不必去比「下发的指令全文」——
    /// 那种比法只看得到主技能，备用技能的新增 / 删除 / 改内容全会漏掉，这正是
    /// 「备用技能没被读取」的来源。
    pub(crate) fn version(&self) -> String {
        let state = self.load_state();
        let mut lines: Vec<String> = Vec::new();

        for (dir_name, path) in self.skill_dirs() {
            let source = path.join("SKILL.md");
            let role = Self::selection(&state, &dir_name).role;
            let metadata = fs::metadata(&source).ok();
            let modified = metadata
                .as_ref()
                .and_then(|value| value.modified().ok())
                .and_then(|value| value.duration_since(SystemTime::UNIX_EPOCH).ok())
                .map(|value| value.as_millis())
                .unwrap_or(0);
            let size = metadata.as_ref().map(|value| value.len()).unwrap_or(0);
            let described = parse_skill_metadata(&source);
            let mut line = format!(
                "{}|{}|{}|{}|{}|{}",
                dir_name,
                role.as_str(),
                size,
                modified,
                described.name.unwrap_or_default(),
                described.description.unwrap_or_default(),
            );
            /* 正文摘要：主技能的正文会整段下发，备用技能会被检索和读取，
             * 两者变了都算版本变化；只有「关闭」的技能不参与对话，不算。 */
            if !matches!(role, SkillRole::None) {
                let content = fs::read_to_string(&source).unwrap_or_default();
                line.push('|');
                line.push_str(&digest(&content));
            }
            lines.push(line);
        }

        lines.sort();
        digest(&lines.join("\n"))
    }

    /// 会话指令：主技能正文直接注入，备用技能只给目录。
    ///
    /// 主技能得是"直接生效的指令"才算主技能 —— 云端网关不一定照做 MCP 工具
    /// 调用，只把名字写进指令里，等于把是否加载全押在模型自觉上。所以正文直接
    /// 放进 `initialize.instructions`，第一轮就带着它；正文被截断时会明确提示用
    /// `skill_read` 续读。备用技能只列名字与说明，等用户问题真的相关时再由模型
    /// `skill_search` / `skill_read` 取用，"按需"这才成立。
    pub(crate) fn instructions(&self) -> Option<String> {
        self.build_instructions()
    }

    fn build_instructions(&self) -> Option<String> {
        let state = self.load_state();
        let mut primaries: Vec<(String, String)> = Vec::new();
        let mut secondary_catalogue = Vec::new();

        for (dir_name, path) in self.skill_dirs() {
            match Self::selection(&state, &dir_name).role {
                SkillRole::Primary => {
                    match fs::read_to_string(path.join("SKILL.md")) {
                        Ok(content) => primaries.push((dir_name, content)),
                        // 读不到正文的主技能等于没生效，必须留下日志
                        Err(error) => {
                            log::warn!("主技能 {} 的 SKILL.md 读取失败，本次未注入：{}", dir_name, error)
                        }
                    }
                }
                SkillRole::Secondary => {
                    let metadata = parse_skill_metadata(&path.join("SKILL.md"));
                    secondary_catalogue.push(format!(
                        "- {}：{}",
                        dir_name,
                        metadata
                            .description
                            .unwrap_or_else(|| "按需检索此 Skill 的内容".to_string())
                    ));
                }
                SkillRole::None => {}
            }
        }

        if primaries.is_empty() {
            return Some(
                "## 主技能缺失\n当前会话没有已启用的主技能。不要直接回答用户请求，也不要把备用技能当作主技能；先提示用户在设备「技能」页或网页控制台启用一个主技能，会话重建后即可继续。"
                    .to_string(),
            );
        }

        // 主技能正文共用一个预算池：先给规则和备用目录留 2000 字符，剩下的按主
        // 技能数量平分，避免靠后的主技能被整体截掉
        let pool = MAX_INSTRUCTIONS_CHARS.saturating_sub(2_000);
        let per_primary = (pool / primaries.len()).clamp(2_000, pool);

        let mut sections = Vec::new();
        for (name, content) in &primaries {
            let total = content.chars().count();
            let clipped = truncate_chars(content, per_primary);
            let loaded = clipped.chars().count();
            if loaded < total {
                // 必须告诉模型“还有后续内容”，否则它会以为技能就只有这些，
                // 表现成“装了技能却不会用”（长 SKILL.md 尤其明显）
                sections.push(format!(
                    "## 主技能（本会话已注入，必须执行）: {}\n{}\n\n（该技能共 {} 字符，本次注入了前 {} 字符。需要后面的内容时，用 skill_read(skill=\"{}\", start_line=…) 续读，或先用 skill_search 检索相关章节；不要凭猜测补全技能内容。）",
                    name, clipped, total, loaded, name
                ));
            } else {
                sections.push(format!(
                    "## 主技能（本会话已注入，必须执行）: {}\n{}",
                    name, content
                ));
            }
        }
        if !secondary_catalogue.is_empty() {
            sections.push(format!(
                "## 备用技能（未注入正文，按需检索）\n{}\n\n只有当用户请求确实与上面某个技能相关时，才先用 skill_search 检索到章节、再用 skill_read 读取原文（长文档用 start_line 续读）。不相关就不要调用，也不要臆造技能内容。",
                secondary_catalogue.join("\n")
            ));
        }

        let primary_names = primaries
            .iter()
            .map(|(name, _)| format!("`{}`", name))
            .collect::<Vec<_>>()
            .join("、");
        sections.insert(0, skill_rules_header(&primary_names));

        Some(truncate_chars(
            &format!(
                "本地技能使用规则：主技能的正文已随本会话注入，必须直接执行；备用技能只在用户问题相关时检索。\n\n{}",
                sections.join("\n\n")
            ),
            MAX_INSTRUCTIONS_CHARS,
        ))
    }

    fn read(
        &self,
        skill: &str,
        relative_path: Option<&str>,
        start_line: Option<usize>,
        max_chars: Option<usize>,
    ) -> Result<Value, String> {
        let dir = self.active_skill_dirs()
            .into_iter()
            .find(|(candidate, _)| candidate == skill)
            .map(|(_, path)| path)
            .ok_or_else(|| format!("Skill '{}' 未启用或不存在", skill))?;
        let relative = relative_path.unwrap_or("SKILL.md");
        let relative_path = Path::new(relative);
        if relative_path.is_absolute()
            || relative_path.components().any(|component| {
                matches!(component, std::path::Component::ParentDir)
            })
        {
            return Err("Invalid Skill path".to_string());
        }
        let path = dir.join(relative_path);
        // Reject symlinks that escape the Skill directory, in addition to the
        // lexical `..` check above.
        let canonical_dir = fs::canonicalize(&dir)
            .map_err(|e| format!("Cannot access Skill directory: {}", e))?;
        let canonical_path = fs::canonicalize(&path)
            .map_err(|e| format!("Cannot read Skill file: {}", e))?;
        if !canonical_path.starts_with(&canonical_dir) {
            return Err("Skill path escapes its directory".to_string());
        }
        let metadata = fs::metadata(&canonical_path)
            .map_err(|e| format!("Cannot read Skill file: {}", e))?;
        if !metadata.is_file() || metadata.len() > MAX_FILE_BYTES {
            return Err("Skill file is missing or too large".to_string());
        }
        let content = fs::read_to_string(&canonical_path)
            .map_err(|e| format!("Cannot decode Skill file as UTF-8: {}", e))?;

        let total_lines = content.lines().count();
        let start = start_line.unwrap_or(1).max(1);
        let limit = max_chars.unwrap_or(MAX_READ_CHARS).clamp(200, MAX_READ_CHARS * 4);

        // 支持按行续读：长文档不再只能读到前 16k 字符
        let picked: String = content
            .lines()
            .skip(start.saturating_sub(1))
            .collect::<Vec<_>>()
            .join("\n");
        let text = truncate_chars(&picked, limit);
        let truncated = text.chars().count() < picked.chars().count();

        Ok(json!({
            "skill": skill,
            "path": relative,
            "start_line": start,
            "total_lines": total_lines,
            "truncated": truncated,
            "next_start_line": if truncated { Some(start + text.matches('\n').count()) } else { None },
            "content": text
        }))
    }

    fn search(&self, skill: Option<&str>, query: &str, limit: usize) -> Result<Value, String> {
        let query = query.trim();
        if query.is_empty() {
            return Err("Missing search query".to_string());
        }
        let dirs = if let Some(name) = skill {
            let dir = self
                .active_skill_dirs()
                .into_iter()
                .find(|(candidate, _)| candidate == name)
                .map(|(_, path)| path)
                .ok_or_else(|| format!("Skill '{}' 未启用或不存在", name))?;
            vec![(name.to_string(), dir)]
        } else {
            self.active_skill_dirs()
        };

        let query_lower = query.to_lowercase();
        let terms = Self::query_terms(&query_lower);
        let limit = limit.clamp(1, MAX_SEARCH_RESULTS);
        let scanned: Vec<String> = dirs.iter().map(|(name, _)| name.clone()).collect();

        let mut matches = Vec::new();
        for (skill_name, dir) in dirs {
            let indexed = self.indexed_skill(&skill_name, &dir);
            /* 技能名的两种写法都算：目录名，以及 SKILL.md 里声明的名字。
             * 用户常直接问「HQQ 怎么写」，这时该技能整体该排前面。 */
            let mut names = vec![skill_name.to_lowercase()];
            if let Some(display) = parse_skill_metadata(&dir.join("SKILL.md")).name {
                let display = display.to_lowercase();
                if !names.contains(&display) {
                    names.push(display);
                }
            }
            for file in &indexed.files {
                for chunk in &file.chunks {
                    let (score, matched) =
                        Self::score_chunk(&query_lower, &terms, chunk, &names);
                    if score == 0 {
                        continue;
                    }
                    let position = Self::first_hit(&chunk.lower, &query_lower, &terms);
                    matches.push(json!({
                        "skill": skill_name,
                        "path": file.relative,
                        "section": chunk.title,
                        "line_start": chunk.line_start,
                        "score": score,
                        "matched": matched,
                        "snippet": make_snippet(&chunk.text, position, MAX_SNIPPET_CHARS)
                    }));
                }
            }
        }

        matches.sort_by(|a, b| {
            b.get("score")
                .and_then(Value::as_u64)
                .cmp(&a.get("score").and_then(Value::as_u64))
        });
        let total = matches.len();

        /* 按 Skill 收敛：高分先取，但同一个技能最多几条 —— 结果列表要回答
         * 「哪个技能、哪一节」，不能让一个技能的相邻章节占满。 */
        let mut kept: Vec<Value> = Vec::new();
        let mut per_skill: BTreeMap<String, usize> = BTreeMap::new();
        for entry in matches {
            let name = entry
                .get("skill")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string();
            let used = per_skill.entry(name).or_insert(0);
            if *used >= MAX_HITS_PER_SKILL {
                continue;
            }
            *used += 1;
            kept.push(entry);
            if kept.len() >= limit {
                break;
            }
        }

        let mut result = json!({
            "query": query,
            "results": kept,
            "count": kept.len(),
            "total_matched": total,
            "skills_matched": per_skill.len()
        });
        if total == 0 {
            // 纯词法检索跨不了同义词。未命中时把「查了什么、扫了哪些」说明白，
            // 调用方才能判断是词不合适、还是该先看看有哪些技能。
            let shown: Vec<String> = terms.iter().take(12).cloned().collect();
            result["hint"] = Value::String(format!(
                "没有章节命中。检索词：{}；已扫描技能：{}。可换更短的关键词重试，先 skill_list 看有哪些技能，或直接用 skill_read 读取 SKILL.md 全文。",
                shown.join(" / "),
                if scanned.is_empty() { "（无）".to_string() } else { scanned.join(" / ") },
            ));
        }
        Ok(result)
    }
}

/// 解压后体积上限，防止 zip 炸弹把分区写满（SPI NAND 启动时生效）
const MAX_SKILL_UNPACKED_BYTES: u64 = 32 * 1024 * 1024;
/// 单个 Skill 的最大文件数（SPI NAND 启动时生效）
const MAX_SKILL_FILES: usize = 512;
/// ZIP 本身上限：解压前要把整包读进内存，受设备 256MB 内存限制，SD 卡启动也保持
const MAX_SKILL_ZIP_BYTES: usize = 64 * 1024 * 1024;

/// 是否从 SD 卡启动：分区落在 /dev/mmcblk* 上说明是（容量大）。
/// SPI NAND 启动时分区在 /dev/mtdblock* 或 /dev/ubiblock* 上，容量只有几百 MB，
/// 必须限制解压体积；SD 卡启动则放开，避免用户装不了稍微大一点的 Skill。
fn booted_from_sd_card() -> bool {
    for name in ["rootfs", "oem", "userdata"] {
        if let Ok(target) = fs::read_link(format!("/dev/block/by-name/{name}")) {
            return target.to_string_lossy().starts_with("/dev/mmcblk");
        }
    }
    // by-name 不存在时退一步看 /proc/mounts
    if let Ok(mounts) = fs::read_to_string("/proc/mounts") {
        for line in mounts.lines() {
            let mut parts = line.split_whitespace();
            let device = parts.next().unwrap_or_default();
            let mount = parts.next().unwrap_or_default();
            if matches!(mount, "/" | "/oem" | "/userdata") && device.starts_with("/dev/mmcblk") {
                return true;
            }
        }
    }
    false
}

/// 解压后体积/文件数上限；SD 卡启动时返回 None 表示不限制
fn unpacked_limits() -> Option<(u64, usize)> {
    if booted_from_sd_card() {
        None
    } else {
        Some((MAX_SKILL_UNPACKED_BYTES, MAX_SKILL_FILES))
    }
}

/// 统计目录内的文件数与总字节数
fn dir_stats(root: &Path) -> (u64, usize) {
    let mut total = 0u64;
    let mut count = 0usize;
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let entries = match fs::read_dir(&dir) {
            Ok(entries) => entries,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            match fs::symlink_metadata(&path) {
                Ok(meta) if meta.file_type().is_dir() => stack.push(path),
                Ok(meta) if meta.file_type().is_file() => {
                    total += meta.len();
                    count += 1;
                }
                _ => {}
            }
        }
    }
    (total, count)
}

/// 解压前用 `unzip -l` 估算解压后体积；解析失败则跳过（后面还有一次实际校验）
async fn check_archive_unpacked_size(archive: &Path) -> Result<(), String> {
    let output = Command::new("unzip")
        .args(["-l", archive.to_str().unwrap_or("/tmp/skill.zip")])
        .output()
        .await
        .map_err(|e| format!("启动 unzip 失败: {}", e))?;
    if !output.status.success() {
        return Err("ZIP 文件无法读取，请确认文件完整".to_string());
    }
    let listing = String::from_utf8_lossy(&output.stdout);
    // 末行形如 "   1234567                     10 files"，第一个字段即解压后总字节数
    let mut declared = None;
    for line in listing.lines().rev() {
        let trimmed = line.trim_start();
        if trimmed.is_empty() || trimmed.starts_with('-') {
            continue;
        }
        if let Some(first) = trimmed.split_whitespace().next() {
            if let Ok(value) = first.replace(',', "").parse::<u64>() {
                declared = Some(value);
                break;
            }
        }
    }
    if let (Some(bytes), Some((max_bytes, _))) = (declared, unpacked_limits()) {
        if bytes > max_bytes {
            return Err(format!(
                "ZIP 解压后约 {} MB，超过 {} MB 上限",
                bytes / 1024 / 1024,
                max_bytes / 1024 / 1024
            ));
        }
    }
    Ok(())
}

fn find_skill_root(root: &Path) -> Option<PathBuf> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let entries = fs::read_dir(&dir).ok()?;
        for entry in entries.flatten() {
            let path = entry.path();
            let meta = fs::symlink_metadata(&path).ok()?;
            if meta.file_type().is_dir() {
                stack.push(path);
            } else if meta.file_type().is_file() && path.file_name().and_then(|v| v.to_str()) == Some("SKILL.md") {
                return path.parent().map(Path::to_path_buf);
            }
        }
    }
    None
}

fn copy_dir(source: &Path, target: &Path) -> std::io::Result<()> {
    fs::create_dir_all(target)?;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let source_path = entry.path();
        let target_path = target.join(entry.file_name());
        let metadata = fs::symlink_metadata(&source_path)?;
        if metadata.file_type().is_dir() {
            copy_dir(&source_path, &target_path)?;
        } else if metadata.file_type().is_file() {
            fs::copy(&source_path, &target_path)?;
        }
    }
    Ok(())
}

fn validate_skill_file(path: &Path) -> Result<(), String> {
    let metadata = fs::metadata(path).map_err(|e| format!("无法读取 SKILL.md: {}", e))?;
    if !metadata.is_file() || metadata.len() == 0 {
        return Err("SKILL.md 为空，无法安装 Skill".to_string());
    }
    if metadata.len() > MAX_FILE_BYTES {
        return Err("SKILL.md 超过 1 MiB 限制".to_string());
    }
    let content = fs::read_to_string(path).map_err(|e| format!("SKILL.md 不是有效的 UTF-8 文件: {}", e))?;
    if content.trim().is_empty() {
        return Err("SKILL.md 为空，无法安装 Skill".to_string());
    }
    Ok(())
}

fn sync_directory(path: &Path) -> std::io::Result<()> {
    fs::File::open(path)?.sync_all()
}

fn validate_skill_name(name: &str) -> Result<(), String> {
    if name.is_empty() || name.len() > 64 || name == "." || name == ".." {
        return Err("Skill 名称长度必须为 1-64 个字符".to_string());
    }
    if !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-' || b == b'.') {
        return Err("Skill 名称只能包含字母、数字、下划线、短横线和点".to_string());
    }
    Ok(())
}

fn collect_markdown_files(dir: &Path, files: &mut Vec<PathBuf>) {
    let entries = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => return,
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let file_type = match fs::symlink_metadata(&path) {
            Ok(metadata) => metadata.file_type(),
            Err(_) => continue,
        };
        if file_type.is_dir() {
            collect_markdown_files(&path, files);
        } else if file_type.is_file() && path.extension().and_then(|e| e.to_str()) == Some("md") {
            files.push(path);
        }
    }
}

struct SkillMetadata {
    name: Option<String>,
    description: Option<String>,
}

fn parse_skill_metadata(path: &Path) -> SkillMetadata {
    let content = match fs::read_to_string(path) {
        Ok(content) => content,
        Err(_) => return SkillMetadata { name: None, description: None },
    };
    let mut name = None;
    let mut description = None;
    for line in content.lines().take(80) {
        let line = line.trim();
        if let Some(value) = line.strip_prefix("name:") {
            name = Some(value.trim().trim_matches('"').to_string());
        } else if let Some(value) = line.strip_prefix("description:") {
            description = Some(value.trim().trim_matches('"').to_string());
        }
        if name.is_some() && description.is_some() {
            break;
        }
    }

    // 没写 description 时取正文第一行有效内容兜底，避免网页里满屏“无描述”
    if description.is_none() {
        for line in content.lines().take(60) {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') || line.starts_with("---") {
                continue;
            }
            // 跳过形如 "author: xxx" 的元数据行
            if let Some((key, _)) = line.split_once(':') {
                if !key.is_empty()
                    && key
                        .chars()
                        .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
                {
                    continue;
                }
            }
            description = Some(truncate_chars(line, 100));
            break;
        }
    }

    SkillMetadata { name, description }
}

fn truncate_chars(value: &str, limit: usize) -> String {
    value.chars().take(limit).collect()
}

fn make_snippet(content: &str, position: usize, limit: usize) -> String {
    let start = position.saturating_sub(limit / 3);
    let end = (start + limit).min(content.len());
    let mut start_boundary = start;
    while start_boundary > 0 && !content.is_char_boundary(start_boundary) {
        start_boundary -= 1;
    }
    let mut end_boundary = end;
    while end_boundary < content.len() && !content.is_char_boundary(end_boundary) {
        end_boundary += 1;
    }
    content[start_boundary..end_boundary].to_string()
}

pub struct SkillListTool {
    manager: SkillManager,
}

impl SkillListTool {
    pub fn new(manager: SkillManager) -> Self { Self { manager } }
}

#[async_trait]
impl McpTool for SkillListTool {
    fn name(&self) -> &str { "skill_list" }
    fn description(&self) -> &str { "列出本会话可用的本地 Skill：主技能（正文已随会话注入，必须执行）和备用技能（按需用 skill_search / skill_read 查阅）。用户在设备上关闭的 Skill 不会出现在这里。" }
    fn input_schema(&self) -> Value { json!({ "type": "object", "properties": {} }) }
    async fn call(&self, _params: Value) -> Result<Value, String> { Ok(self.manager.list_for_model()) }
}

pub struct SkillSearchTool {
    manager: SkillManager,
}

impl SkillSearchTool {
    pub fn new(manager: SkillManager) -> Self { Self { manager } }
}

#[async_trait]
impl McpTool for SkillSearchTool {
    fn name(&self) -> &str { "skill_search" }
    fn description(&self) -> &str { "按用户请求检索本地 Skill 的具体章节。主技能正文已在会话里，只有需要参考文件、或技能正文被截断的后半部分时才检索；备用技能在被明确需要时检索。返回结果按 Markdown 章节切分，包含 section（章节标题）、path（文件）与 line_start（起始行号），命中后用 skill_read 读取原文。" }
    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "required": ["query"],
            "properties": {
                "skill": { "type": "string", "description": "可选 Skill 名称，不填则检索全部可用 Skill" },
                "query": { "type": "string", "description": "要检索的关键词或问题，支持中文" },
                "limit": { "type": "integer", "description": "最多返回的章节数，默认 6，最大 6" }
            }
        })
    }
    async fn call(&self, params: Value) -> Result<Value, String> {
        let query = params.get("query").and_then(Value::as_str).unwrap_or_default();
        let skill = params.get("skill").and_then(Value::as_str);
        let limit = params.get("limit").and_then(Value::as_u64).unwrap_or(MAX_SEARCH_RESULTS as u64);
        self.manager.search(skill, query, limit as usize)
    }
}

pub struct SkillReadTool {
    manager: SkillManager,
}

impl SkillReadTool {
    pub fn new(manager: SkillManager) -> Self { Self { manager } }
}

#[async_trait]
impl McpTool for SkillReadTool {
    fn name(&self) -> &str { "skill_read" }
    fn description(&self) -> &str { "读取本地 Skill 的 SKILL.md 或检索结果指定的参考文件。只能读主技能与备用技能（用户在设备上关闭的 Skill 不可读）。支持用 start_line 从指定行开始续读，返回内容中会给出 total_lines / truncated / next_start_line，便于分次读完长文档。" }
    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "required": ["skill"],
            "properties": {
                "skill": { "type": "string" },
                "path": { "type": "string", "description": "相对 Skill 目录的 Markdown 路径，默认 SKILL.md" },
                "start_line": { "type": "integer", "description": "从第几行开始读（1 起始），默认 1" },
                "max_chars": { "type": "integer", "description": "本次最多返回的字符数，默认 16000" }
            }
        })
    }
    async fn call(&self, params: Value) -> Result<Value, String> {
        let skill = params.get("skill").and_then(Value::as_str).ok_or("Missing skill name")?;
        let path = params.get("path").and_then(Value::as_str);
        let start_line = params.get("start_line").and_then(Value::as_u64).map(|v| v as usize);
        let max_chars = params.get("max_chars").and_then(Value::as_u64).map(|v| v as usize);
        self.manager.read(skill, path, start_line, max_chars)
    }
}

/// 会话里切换技能：用户说「换成中医模式」「用查资料那个技能回答」「别用这个人设了」时调用。
///
/// 以前换主技能只能去网页控制台改，而改完核心会**重建会话**（WebSocket 断线重连）
/// 才能把新的正文送上去 —— 换一句话的人设要付一次重连的代价，对话也断在那里。
/// 这个工具把整件事收进当前会话：
///
///   1. 落盘角色：与网页控制台改的是同一份选择，重连或重启后依然生效；
///   2. 把切换后的技能正文**直接放进返回值**，这一轮就生效，不必等重连；
///   3. 设为主技能时把原主技能降为备用（见 `primary_switch_plan`）—— 切换而不是
///      叠加，两套人设同时注入会互相打架。
pub struct SkillUseTool {
    manager: SkillManager,
}

impl SkillUseTool {
    pub fn new(manager: SkillManager) -> Self {
        Self { manager }
    }
}

#[async_trait]
impl McpTool for SkillUseTool {
    fn name(&self) -> &str {
        "skill_use"
    }

    fn description(&self) -> &str {
        "在当前会话里切换正在使用的 Skill（人设 / 知识库），不需要重建会话、不会掉线。\
         用户说「换成××模式」「用××技能回答」「别用这个人设了」「把它改成备用」时调用。\
         name 是技能名（先用 skill_list 查准确名字）；role 默认 primary —— 设为主技能，\
         原来的主技能自动降为备用；也可以给 secondary 或 none。返回的 instructions 是\
         切换后当前生效的主技能正文，以它为准。"
    }

    fn input_schema(&self) -> Value {
        json!({
            "type": "object",
            "required": ["name"],
            "properties": {
                "name": { "type": "string", "description": "Skill 目录名，先用 skill_list 查" },
                "role": {
                    "type": "string",
                    "enum": ["primary", "secondary", "none"],
                    "description": "切换成什么角色，默认 primary"
                }
            }
        })
    }

    async fn call(&self, params: Value) -> Result<Value, String> {
        let name = params
            .get("name")
            .and_then(Value::as_str)
            .unwrap_or("")
            .trim()
            .to_string();
        if name.is_empty() {
            return Ok(json!({ "status": "error", "message": "name 不能为空" }));
        }
        let role_value = params
            .get("role")
            .and_then(Value::as_str)
            .unwrap_or("primary")
            .trim();
        let role = match SkillRole::parse(role_value) {
            Ok(role) => role,
            Err(message) => return Ok(json!({ "status": "error", "message": message })),
        };

        let effect = match role {
            SkillRole::Primary => self.manager.switch_primary(&name),
            other => self.manager.set_role(&name, other),
        };
        if let Err(message) = effect {
            /* 名字写错是最常见的失败：把可用列表一起带回去，模型下一轮就能改对，
             * 不用再单独调一次 skill_list。 */
            return Ok(json!({
                "status": "error",
                "message": message,
                "skills": self.manager.list_for_model(),
            }));
        }

        let label = match role {
            SkillRole::Primary => "主技能（正文已随本次返回给你，现在起按它回答）",
            SkillRole::Secondary => "备用（不占上下文，需要时用 skill_search / skill_read 检索）",
            SkillRole::None => "关闭（不再参与对话）",
        };
        log::info!(
            "skill_use: {} -> {}（会话内直接生效，未重建会话）",
            name,
            role.as_str()
        );
        Ok(json!({
            "status": "ok",
            "skill": name,
            "role": role.as_str(),
            "message": format!("已切换：{} 现在是{}", name, label),
            /* 切换后的完整指令块（主技能正文 + 备用目录 + 使用规则）。它比会话开始时
             * 随 skill_list 描述注入的那一份新，所以必须把优先级写清楚，否则模型会
             * 同时看到两套人设。 */
            "instructions": self.manager.instructions().unwrap_or_default(),
            "precedence": "上面的 instructions 是本会话此刻生效的主技能与备用目录，\
                           优先级高于会话开始时注入的那一份：以它为准，不要复述它，直接按它回答。",
        }))
    }
}

/// 诊断：现在的 Skill 配置到底是什么、有没有真的生效。
///
/// 之前排查「主技能没完全生效」「备用技能没被读取」只能翻日志；这个工具让模型
/// （或人）直接问设备：主/备用各是哪些、版本是多少、正文实际注入了多少字符。
pub struct SkillStatusTool {
    manager: SkillManager,
}

impl SkillStatusTool {
    pub fn new(manager: SkillManager) -> Self {
        Self { manager }
    }
}

#[async_trait]
impl McpTool for SkillStatusTool {
    fn name(&self) -> &str {
        "skill_status"
    }
    fn description(&self) -> &str {
        "查看当前 Skill 配置诊断信息：主技能与备用技能清单、配置版本、主技能正文实际注入的字符数、状态文件位置。用户问「现在用的是哪个技能」「技能到底有没有生效」「为什么没按技能回答」时调用。"
    }
    fn input_schema(&self) -> Value {
        json!({ "type": "object", "properties": {} })
    }
    async fn call(&self, _params: Value) -> Result<Value, String> {
        let manager = &self.manager;
        let state = manager.load_state();
        let mut primary = Vec::new();
        let mut secondary = Vec::new();

        for (dir_name, path) in manager.skill_dirs() {
            let role = SkillManager::selection(&state, &dir_name).role;
            let described = parse_skill_metadata(&path.join("SKILL.md"));
            let entry = json!({
                "skill": dir_name,
                "name": described.name.unwrap_or_default(),
                "description": described.description.unwrap_or_default(),
                "path": path.display().to_string(),
                "writable": manager.is_user_installed(&dir_name),
            });
            match role {
                SkillRole::Primary => primary.push(entry),
                SkillRole::Secondary => secondary.push(entry),
                SkillRole::None => {}
            }
        }

        let instructions = manager.instructions().unwrap_or_default();
        Ok(json!({
            "version": manager.version(),
            "primary": primary,
            "secondary": secondary,
            "instructions_chars": instructions.chars().count(),
            "instructions_budget_chars": MAX_INSTRUCTIONS_CHARS,
            "state_file": manager.state_path().display().to_string(),
            "note": "配置版本变化后，下一次唤醒会自动重建会话，让云端按新配置重新 initialize。"
        }))
    }
}

/// 体检：逐个技能实际读一遍、建一次索引，把问题直接列出来。
pub struct SkillValidateTool {
    manager: SkillManager,
}

impl SkillValidateTool {
    pub fn new(manager: SkillManager) -> Self {
        Self { manager }
    }
}

#[async_trait]
impl McpTool for SkillValidateTool {
    fn name(&self) -> &str {
        "skill_validate"
    }
    fn description(&self) -> &str {
        "体检当前生效的 Skill：SKILL.md 是否读得到、能否建立检索索引、说明是否齐全，并列出具体问题。用户反馈「装了技能但用不起来」「检索不到内容」时调用。"
    }
    fn input_schema(&self) -> Value {
        json!({ "type": "object", "properties": {} })
    }
    async fn call(&self, _params: Value) -> Result<Value, String> {
        let manager = &self.manager;
        let state = manager.load_state();
        let mut reports = Vec::new();
        let mut healthy = true;

        for (dir_name, path) in manager.skill_dirs() {
            let role = SkillManager::selection(&state, &dir_name).role;
            if matches!(role, SkillRole::None) {
                // 关闭的技能不参与对话，也就不必体检
                continue;
            }
            let source = path.join("SKILL.md");
            let mut problems: Vec<String> = Vec::new();
            let indexed = manager.indexed_skill(&dir_name, &path);
            let files = indexed.files.len();
            let chunks: usize = indexed.files.iter().map(|file| file.chunks.len()).sum();
            if files == 0 {
                problems.push("没有可索引的 Markdown：SKILL.md 缺失或读不出来".to_string());
            } else if chunks == 0 {
                problems.push("没有可分块检索的章节：检索与续读都会落空".to_string());
            }
            if parse_skill_metadata(&source)
                .description
                .unwrap_or_default()
                .trim()
                .is_empty()
            {
                problems.push(
                    "缺少 description：模型只看得到名字，备用技能很难被检索到".to_string(),
                );
            }

            if !problems.is_empty() {
                healthy = false;
            }
            reports.push(json!({
                "skill": dir_name,
                "role": role.as_str(),
                "files": files,
                "chunks": chunks,
                "readable": files > 0,
                "searchable": chunks > 0,
                "problems": problems,
            }));
        }

        Ok(json!({
            "ok": healthy,
            "version": manager.version(),
            "skills": reports,
        }))
    }
}

/// 重新扫描：改过文件、装过包之后让索引立刻跟上。
pub struct SkillReloadTool {
    manager: SkillManager,
}

impl SkillReloadTool {
    pub fn new(manager: SkillManager) -> Self {
        Self { manager }
    }
}

#[async_trait]
impl McpTool for SkillReloadTool {
    fn name(&self) -> &str {
        "skill_reload"
    }
    fn description(&self) -> &str {
        "重新扫描本地 Skill 目录并重建检索索引。改过 SKILL.md、新装或删过技能之后调用，之后立刻能用 skill_search 检索到新内容；返回新的配置版本。"
    }
    fn input_schema(&self) -> Value {
        json!({ "type": "object", "properties": {} })
    }
    async fn call(&self, _params: Value) -> Result<Value, String> {
        let manager = &self.manager;
        manager.invalidate_index();
        let active = manager.active_skill_dirs();
        let skills: Vec<Value> = active
            .iter()
            .map(|(name, path)| {
                let indexed = manager.indexed_skill(name, path);
                let chunks: usize = indexed.files.iter().map(|file| file.chunks.len()).sum();
                json!({ "skill": name, "chunks": chunks })
            })
            .collect();
        Ok(json!({
            "ok": true,
            "version": manager.version(),
            "reloaded": skills,
        }))
    }
}

/// 导入后新技能该拿到的角色：只有**本次新出现、且还没有角色记录**的才补一个
/// 主技能。
///
/// 两种情况都不动：用户选过「备用 / 关闭」（不该因为一次重新导入被改回主技能），
/// 以及覆盖已存在的技能（编辑一个预置技能不该顺手把它打开、塞进每轮上下文）。
///
/// 纯函数，方便测试直接盯住「导入即生效，但不覆盖用户的选择」这条规则。
fn role_for_newly_installed(
    state: &SkillState,
    name: &str,
    existed_before: bool,
) -> Option<SkillRole> {
    if existed_before || state.skills.contains_key(name) {
        return None;
    }
    Some(SkillRole::Primary)
}

/// 「换成某个技能」时要写下的角色变化：目标升为主技能，原来的主技能降为备用。
///
/// 是**切换**而不是叠加：两个主技能会同时把正文注入同一个会话，两套人设互相打架，
/// 用户说「换成中医模式」时显然不想要这个结果。用户显式关掉的技能不在计划里 ——
/// 换主技能不该顺手把谁打开。
///
/// 纯函数，规则直接可测；`discovered` 传磁盘上扫到的技能名（与
/// `build_instructions` 看到的是同一批），免得动到已经不存在的条目。
fn primary_switch_plan(
    state: &SkillState,
    discovered: &[String],
    name: &str,
) -> Vec<(String, SkillRole)> {
    let mut plan = Vec::new();
    for dir_name in discovered {
        if dir_name == name {
            continue;
        }
        if SkillManager::selection(state, dir_name).role == SkillRole::Primary {
            plan.push((dir_name.clone(), SkillRole::Secondary));
        }
    }
    plan.push((name.to_string(), SkillRole::Primary));
    plan
}

/// 注入给模型的头一段：技能规则的硬约束。
///
/// 抽成独立函数是为了能被测试盯住——这几条是「AI 是否真的遵守 Skill」的兜底：
/// 人格优先（别回到默认自我介绍）、不假装调用工具（别把工具名写进正文）、
/// 不谎报技能状态（没查过就不许说「已启用」）。
fn skill_rules_header(primary_names: &str) -> String {
    format!(
        "## 本地技能规则（最高优先级，覆盖默认人格）\n\
         本会话的主技能是：{}。它的正文已随本会话直接注入，按下面四条执行：\n\
         1. **身份优先**：从本会话第一句话起，就用主技能规定的人格、语气与格式回答。不要自称「QZdesk 的智能助手」或其它默认身份，也不要先说一句中性寒暄再进入角色；技能定了说话方式就照它说。\n\
         2. **不假装调用工具**：需要技能里的信息时，必须真正发起工具调用。不要把工具名或调用写法当正文写出来（例如写出 skill_list、skill_search、或以 % 开头的调用标记），也不要说「我查一下」之后却把结果编出来。\n\
         3. **不谎报状态**：凡是「某个技能有没有导入 / 有没有启用 / 当前主技能是哪个 / 技能内容有没有生效」这类问题，答案只能来自 skill_status 或 skill_list 的真实返回。没有实际调用过就不得声称「已导入」「已启用」；不确定就直说不确定，并提示用户可以在设备「技能」页或网页控制台核对。\n\
         4. 不要复述技能原文，也不要解释这段注入内容。需要参考文件、或技能正文被截断的后半部分时，用 skill_read / skill_search 取用；备用技能只在用户问题确实相关时检索，不要凭空补全。\n\
         主技能之间有冲突时按其共同约定处理；涉及安全或用户明确更改任务时，遵循更高优先级规则。",
        primary_names
    )
}

#[cfg(test)]
mod tests {
    #[test]
    fn injected_rules_demand_persona_and_forbid_fabricating() {
        let header = super::skill_rules_header("`HQQ`");
        // 主技能名字要出现，模型才知道它是谁
        assert!(header.contains("`HQQ`"));
        for needle in ["身份优先", "QZdesk 的智能助手", "不假装调用工具", "不谎报状态"] {
            assert!(header.contains(needle), "规则里缺少「{}」", needle);
        }
        // 「已启用/已导入」必须要有工具结果支撑，这句话不能被改掉
        assert!(header.contains("skill_status"));
        assert!(header.contains("不得声称"));
    }
    use super::*;

    #[test]
    fn imported_skill_becomes_primary_unless_the_user_already_chose() {
        let mut state = SkillState::default();
        // 从没见过的名字：导入即主技能，第一句对话就能按它回答
        assert_eq!(
            role_for_newly_installed(&state, "tcm", false),
            Some(SkillRole::Primary)
        );
        // 覆盖已有的技能（编辑 / 重新上传）：角色交给用户，不顺手打开
        assert_eq!(role_for_newly_installed(&state, "tcm", true), None);
        // 用户选过「备用」：重新导入 / 编辑不改回去
        state.skills.insert(
            "tcm".to_string(),
            SkillSelection {
                role: SkillRole::Secondary,
            },
        );
        assert_eq!(role_for_newly_installed(&state, "tcm", false), None);
        // 用户显式关闭过：同样不动
        state.skills.insert(
            "tcm".to_string(),
            SkillSelection {
                role: SkillRole::None,
            },
        );
        assert_eq!(role_for_newly_installed(&state, "tcm", false), None);
    }

    #[test]
    fn switching_a_skill_replaces_the_primary_instead_of_stacking() {
        let mut state = SkillState::default();
        state.skills.insert(
            "hqq".to_string(),
            SkillSelection {
                role: SkillRole::Primary,
            },
        );
        state.skills.insert(
            "tcm".to_string(),
            SkillSelection {
                role: SkillRole::Secondary,
            },
        );
        state.skills.insert(
            "closed".to_string(),
            SkillSelection {
                role: SkillRole::None,
            },
        );
        let discovered: Vec<String> = ["hqq", "tcm", "closed"]
            .iter()
            .map(|name| name.to_string())
            .collect();

        let plan = primary_switch_plan(&state, &discovered, "tcm");
        assert!(
            plan.contains(&("hqq".to_string(), SkillRole::Secondary)),
            "原主技能要降为备用，否则两套人设会同时注入"
        );
        assert!(plan.contains(&("tcm".to_string(), SkillRole::Primary)));
        assert!(
            !plan.iter().any(|(name, _)| name == "closed"),
            "用户显式关掉的技能不该被顺手打开"
        );

        // 当前没有主技能时也一样：只有目标升为主技能
        let plan = primary_switch_plan(&SkillState::default(), &discovered, "hqq");
        assert_eq!(plan, vec![("hqq".to_string(), SkillRole::Primary)]);
    }

    /// `skill_use` 真正要保证的三件事：正文随返回值下发（不必等重连）、选择落盘、
    /// 原主技能降级（不叠加）。
    #[tokio::test]
    async fn skill_use_switches_the_primary_and_hands_back_its_body() {
        let dir = env::temp_dir().join(format!("qzdesk-skill-use-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("hqq")).unwrap();
        fs::create_dir_all(dir.join("tcm")).unwrap();
        fs::write(dir.join("hqq").join("SKILL.md"), "# HQQ\n你是 HQQ 模式。").unwrap();
        fs::write(dir.join("tcm").join("SKILL.md"), "# 中医\n你是中医模式。").unwrap();

        let manager = SkillManager::for_test_dir(dir.clone());
        manager.set_role("hqq", SkillRole::Primary).unwrap();
        manager.set_role("tcm", SkillRole::Secondary).unwrap();

        let tool = SkillUseTool::new(manager.clone());
        let switched = tool.call(json!({ "name": "tcm" })).await.unwrap();
        assert_eq!(switched["status"], "ok");
        assert_eq!(switched["role"], "primary");
        // 正文就在返回值里：模型这一轮就能按它回答，不需要重建会话
        assert!(
            switched["instructions"]
                .as_str()
                .unwrap()
                .contains("你是中医模式。"),
            "切换后的主技能正文应当随工具结果下发"
        );

        // 选择落盘了：重连 / 重启后依然是这个选择
        let state = manager.load_state();
        assert_eq!(
            state.skills.get("tcm").map(|item| item.role),
            Some(SkillRole::Primary)
        );
        assert_eq!(
            state.skills.get("hqq").map(|item| item.role),
            Some(SkillRole::Secondary)
        );

        // 名字写错时把可用列表带回去，模型下一轮就能改对
        let wrong = tool.call(json!({ "name": "不存在的技能" })).await.unwrap();
        assert_eq!(wrong["status"], "error");
        assert!(!wrong["skills"].is_null(), "要顺带告诉模型有哪些技能可选");

        let _ = fs::remove_dir_all(&dir);
    }

    fn chunk(title: &str, text: &str) -> IndexedChunk {
        IndexedChunk {
            title: title.to_string(),
            line_start: 1,
            text: text.to_string(),
            lower: text.to_lowercase(),
        }
    }

    #[test]
    fn chinese_query_gets_bigrams_and_trigrams() {
        let terms = SkillManager::query_terms("查询天气预报");
        assert!(terms.contains(&"查询天气预报".to_string()), "整句本身就是一个词");
        assert!(terms.contains(&"天气".to_string()), "二元组应保召回");
        assert!(terms.contains(&"天气预".to_string()), "三元组滑窗应保精度");
        // 去重后再没有重复项
        let mut unique = terms.clone();
        unique.dedup();
        assert_eq!(unique.len(), terms.len());
    }

    #[test]
    fn ascii_words_stay_whole() {
        let terms = SkillManager::query_terms("how to write hqq");
        assert!(terms.contains(&"how".to_string()));
        assert!(terms.contains(&"hqq".to_string()));
    }

    #[test]
    fn longer_terms_weigh_more() {
        assert!(SkillManager::term_weight("天气预报") > SkillManager::term_weight("天气"));
        assert!(SkillManager::term_weight("天气预报") > SkillManager::term_weight("hqq"));
        assert!(SkillManager::term_weight("hqq") > 0);
    }

    #[test]
    fn title_and_body_hits_score_above_body_only() {
        let terms = SkillManager::query_terms("天气");
        let names = vec!["weather".to_string()];
        let titled = chunk("天气预报", "这里讲各种天气");
        let body_only = chunk("其它章节", "这里提到天气一次");

        let (titled_score, matched) = SkillManager::score_chunk("天气", &terms, &titled, &names);
        let (body_score, _) = SkillManager::score_chunk("天气", &terms, &body_only, &names);
        assert!(titled_score > body_score, "标题命中应排前面");
        assert!(!matched.is_empty(), "命中词要一起返回，便于诊断");
    }

    #[test]
    fn whole_phrase_beats_scattered_terms() {
        let terms = SkillManager::query_terms("天气 预报");
        let names = vec!["weather".to_string()];
        let phrase = chunk("说明", "天气预报");
        let scattered = chunk("说明", "天气……很多段落……预报");

        let (phrase_score, _) = SkillManager::score_chunk("天气 预报", &terms, &phrase, &names);
        let (scattered_score, _) =
            SkillManager::score_chunk("天气 预报", &terms, &scattered, &names);
        assert!(phrase_score > scattered_score);
    }

    #[test]
    fn naming_the_skill_boosts_every_chunk_of_it() {
        let terms = SkillManager::query_terms("hqq 怎么写");
        let target = chunk("开头", "随便一句话");
        let named = SkillManager::score_chunk("hqq 怎么写", &terms, &target, &["hqq".to_string()]);
        let unnamed = SkillManager::score_chunk("hqq 怎么写", &terms, &target, &[String::new()]);
        assert!(named.0 > unnamed.0, "点名技能时该技能整体该加权");
    }

    #[test]
    fn digest_tracks_content_changes() {
        assert_eq!(digest("同样的内容"), digest("同样的内容"));
        assert_ne!(digest("第一版"), digest("第二版"));
        assert_eq!(digest("内容").len(), 16);
    }

    #[test]
    fn version_is_stable_when_nothing_changes() {
        // 没有技能时版本也必须是确定值：调用两次应当一致（会话据此判断是否需要重建）
        let manager = SkillManager::new();
        assert_eq!(manager.version(), manager.version());
    }
}
