//! Local, file-backed Skill retrieval for the MCP gateway.
//!
//! A Skill is a directory containing SKILL.md plus optional Markdown
//! references.  The directory is scanned on every request, so installing or
//! removing a Skill takes effect without rebuilding the Rust binary.

use crate::archive::StoredZip;
use async_trait::async_trait;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};
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
const BUNDLE_FORMAT: &str = "qzdesk.skills";
const BUNDLE_VERSION: u32 = 1;
const BUNDLE_MANIFEST: &str = "qzdesk-skills.json";
/// 技能包要整个读进内存再通过网页返回，因此与单 Skill ZIP 共用 64 MiB 上限。
const MAX_BUNDLE_CONTENT_BYTES: u64 = 60 * 1024 * 1024;
const MAX_BUNDLE_FILES: usize = 4_096;

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
        Self {
            role: SkillRole::None,
        }
    }
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
struct SkillState {
    #[serde(default)]
    skills: BTreeMap<String, SkillSelection>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct SkillBundleManifest {
    format: String,
    version: u32,
    #[serde(default)]
    exported_at: u64,
    skills: Vec<SkillBundleEntry>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct SkillBundleEntry {
    id: String,
    #[serde(default)]
    name: String,
    #[serde(default)]
    description: String,
    #[serde(default)]
    role: SkillRole,
    #[serde(default)]
    files: usize,
    #[serde(default)]
    bytes: u64,
}

#[derive(Debug)]
pub(crate) struct SkillBundleImportError {
    pub(crate) message: String,
    pub(crate) conflicts: Vec<String>,
}

impl SkillBundleImportError {
    fn invalid(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
            conflicts: Vec::new(),
        }
    }
}

struct BundleFile {
    relative: String,
    path: PathBuf,
    size: u64,
}

struct BundleImportPlan {
    id: String,
    role: SkillRole,
    source: PathBuf,
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
    /// 测试里把“用户安装池”固定到临时目录；生产环境按配置或默认路径推导。
    install_root_override: Option<PathBuf>,
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
            install_root_override: None,
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
            install_root_override: Some(dir.clone()),
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
        let content =
            serde_json::to_vec_pretty(state).map_err(|e| format!("无法编码 Skill 状态: {}", e))?;
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
                title = if heading.is_empty() {
                    String::from("概述")
                } else {
                    heading.to_string()
                };
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
            c.is_whitespace()
                || matches!(
                    c,
                    ',' | '，' | '。' | '、' | '?' | '？' | '!' | '！' | ':' | '：' | ';' | '；'
                )
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
                include_disabled || value.get("role").and_then(Value::as_str) != Some("none")
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
        if let Some(path) = &self.install_root_override {
            return path.clone();
        }
        if let Some(path) = configured_skill_root() {
            return PathBuf::from(path);
        }
        default_user_skill_root()
    }

    /// 用户安装池里的 Skill。导出必须直接从这里取，不能走 `skill_dirs`：
    /// 后者允许只读源码目录按优先级覆盖同名项，可能把源码正文误当成用户版本。
    fn user_skill_dirs(&self) -> Vec<(String, PathBuf)> {
        let root = self.install_root();
        let entries = match fs::read_dir(&root) {
            Ok(entries) => entries,
            Err(_) => return Vec::new(),
        };
        let mut result = Vec::new();
        for entry in entries.flatten() {
            let name = entry.file_name();
            let Some(name) = name.to_str() else {
                continue;
            };
            if is_bundle_cache_component(name) || validate_skill_name(name).is_err() {
                continue;
            }
            let path = entry.path();
            match fs::symlink_metadata(&path) {
                Ok(metadata) if metadata.file_type().is_dir() => {}
                _ => continue,
            }
            match fs::symlink_metadata(path.join("SKILL.md")) {
                Ok(metadata) if metadata.file_type().is_file() => {}
                _ => continue,
            }
            result.push((name.to_string(), path));
        }
        result.sort_by(|left, right| left.0.cmp(&right.0));
        result
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
        file.write_all(skill_md.as_bytes())
            .map_err(|e| format!("无法写入 SKILL.md: {}", e))?;
        file.sync_all()
            .map_err(|e| format!("无法保存 SKILL.md: {}", e))?;
        fs::rename(&tmp, dir.join("SKILL.md")).map_err(|e| format!("无法提交 SKILL.md: {}", e))?;
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
        state
            .skills
            .insert(name.to_string(), SkillSelection { role });
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
        state
            .skills
            .insert(name.to_string(), SkillSelection { role });
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

    /// 把用户安装的全部 Skill 打包成一个可离线搬运的 ZIP。
    ///
    /// OEM / 源码目录里的技能不进入包内，避免把固件内容复制成用户覆盖层；
    /// 用户安装池里的关闭项也会导出，因为角色选择本身就是迁移内容的一部分。
    pub(crate) fn export_bundle(&self) -> Result<Vec<u8>, String> {
        let state = self.load_state();
        let exported_at = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|duration| duration.as_secs())
            .unwrap_or(0);
        let mut manifest = SkillBundleManifest {
            format: BUNDLE_FORMAT.to_string(),
            version: BUNDLE_VERSION,
            exported_at,
            skills: Vec::new(),
        };
        let mut archive = StoredZip::new();
        let mut total_bytes = 0u64;
        let mut total_files = 0usize;

        for (id, dir) in self.user_skill_dirs() {
            validate_skill_name(&id)?;
            validate_skill_file(&dir.join("SKILL.md"))?;
            let files = collect_bundle_files(&dir)?;
            if !files.iter().any(|file| file.relative == "SKILL.md") {
                return Err(format!("Skill '{}' 缺少 SKILL.md", id));
            }
            let skill_bytes = files.iter().try_fold(0u64, |total, file| {
                total
                    .checked_add(file.size)
                    .ok_or_else(|| format!("Skill '{}' 体积超出限制", id))
            })?;
            total_bytes = total_bytes
                .checked_add(skill_bytes)
                .ok_or_else(|| "技能包体积超出限制".to_string())?;
            total_files = total_files
                .checked_add(files.len())
                .ok_or_else(|| "技能包文件数超出限制".to_string())?;
            if total_bytes > MAX_BUNDLE_CONTENT_BYTES || total_files > MAX_BUNDLE_FILES {
                return Err(format!(
                    "技能包超过 {} MiB / {} 个文件上限，请先移除过大的 Skill",
                    MAX_BUNDLE_CONTENT_BYTES / 1024 / 1024,
                    MAX_BUNDLE_FILES
                ));
            }

            let metadata = parse_skill_metadata(&dir.join("SKILL.md"));
            manifest.skills.push(SkillBundleEntry {
                id: id.clone(),
                name: metadata.name.unwrap_or_else(|| id.clone()),
                description: metadata.description.unwrap_or_default(),
                role: Self::selection(&state, &id).role,
                files: files.len(),
                bytes: skill_bytes,
            });
            for file in files {
                let content = fs::read(&file.path).map_err(|error| {
                    format!("读取 Skill 文件 '{}' 失败: {}", file.relative, error)
                })?;
                archive.add(&format!("skills/{}/{}", id, file.relative), &content)?;
            }
        }

        if manifest.skills.is_empty() {
            return Err("没有可导出的用户安装 Skill".to_string());
        }
        let manifest = serde_json::to_vec_pretty(&manifest)
            .map_err(|error| format!("无法编码技能包清单: {}", error))?;
        archive.add(BUNDLE_MANIFEST, &manifest)?;
        let bundle = archive.finish()?;
        if bundle.len() as u64 > MAX_SKILL_ZIP_BYTES as u64 {
            return Err(format!(
                "技能包超过 {} MiB 上限",
                MAX_SKILL_ZIP_BYTES / 1024 / 1024
            ));
        }
        Ok(bundle)
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
            .args([
                "-q",
                archive.to_str().unwrap_or("skill.zip"),
                "-d",
                extract_dir.to_str().unwrap_or("extract"),
            ])
            .status()
            .await
            .map_err(|e| format!("启动 unzip 失败: {}", e))?;
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
        let source =
            find_skill_root(&extract_dir).ok_or_else(|| "ZIP 中没有找到 SKILL.md".to_string())?;
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

    /// 导入整套技能包。先完整预检，再复制到安装目录旁的 staging 区，最后逐项提交。
    ///
    /// 同名 Skill 默认返回冲突列表，网页确认后才带 `replace_existing = true`
    /// 重试。旧版本会在提交后保留到备份区，状态落盘失败时用于回滚。
    pub(crate) async fn import_bundle(
        &self,
        archive_data: &[u8],
        replace_existing: bool,
    ) -> Result<Value, SkillBundleImportError> {
        if archive_data.is_empty() {
            return Err(SkillBundleImportError::invalid("技能包为空"));
        }
        if archive_data.len() > MAX_SKILL_ZIP_BYTES {
            return Err(SkillBundleImportError::invalid(format!(
                "技能包超过 {} MiB 上限",
                MAX_SKILL_ZIP_BYTES / 1024 / 1024
            )));
        }
        validate_bundle_zip(archive_data).map_err(SkillBundleImportError::invalid)?;

        let token = uuid::Uuid::new_v4().simple().to_string();
        let install_root = self.install_root();
        let temp = if booted_from_sd_card() {
            install_root
                .parent()
                .map(Path::to_path_buf)
                .unwrap_or_else(std::env::temp_dir)
                .join(format!(".qzdesk-bundle-import-{}", token))
        } else {
            std::env::temp_dir().join(format!("qzdesk-bundle-import-{}", token))
        };
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).map_err(|error| {
            SkillBundleImportError::invalid(format!("无法创建临时目录: {}", error))
        })?;
        let archive = temp.join("bundle.zip");
        fs::write(&archive, archive_data).map_err(|error| {
            SkillBundleImportError::invalid(format!("无法保存技能包: {}", error))
        })?;

        let extract_dir = temp.join("extract");
        fs::create_dir_all(&extract_dir).map_err(|error| {
            SkillBundleImportError::invalid(format!("无法创建解压目录: {}", error))
        })?;
        let extract = Command::new("unzip")
            .arg("-q")
            .arg("-o")
            .arg(&archive)
            .arg("-d")
            .arg(&extract_dir)
            .status()
            .await;
        match extract {
            Ok(status) if status.success() => {}
            Ok(_) => {
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(
                    "技能包解压失败，请确认文件完整",
                ));
            }
            Err(error) => {
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(format!(
                    "启动 unzip 失败: {}",
                    error
                )));
            }
        }
        if let Err(error) = validate_extracted_bundle_tree(&extract_dir) {
            let _ = fs::remove_dir_all(&temp);
            return Err(error);
        }

        let manifest_path = extract_dir.join(BUNDLE_MANIFEST);
        let manifest_size = fs::metadata(&manifest_path)
            .map_err(|_| SkillBundleImportError::invalid("技能包缺少 qzdesk-skills.json 清单"))?
            .len();
        if manifest_size > MAX_FILE_BYTES {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid("技能包清单超过 1 MiB"));
        }
        let manifest_data = match fs::read(&manifest_path) {
            Ok(data) => data,
            Err(error) => {
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(format!(
                    "无法读取技能包清单: {}",
                    error
                )));
            }
        };
        let manifest: SkillBundleManifest = match serde_json::from_slice(&manifest_data) {
            Ok(manifest) => manifest,
            Err(error) => {
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(format!(
                    "技能包清单格式错误: {}",
                    error
                )));
            }
        };
        if manifest.format != BUNDLE_FORMAT {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid("不是 QZdesk 技能包"));
        }
        if manifest.version != BUNDLE_VERSION {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid(format!(
                "不支持技能包版本 {}，当前支持版本 {}",
                manifest.version, BUNDLE_VERSION
            )));
        }
        if manifest.skills.is_empty() {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid("技能包里没有技能"));
        }
        if manifest.skills.len() > MAX_BUNDLE_FILES {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid(
                "技能包中的技能数量超出限制",
            ));
        }

        let mut seen = BTreeMap::new();
        let mut plans = Vec::with_capacity(manifest.skills.len());
        for entry in &manifest.skills {
            if let Err(error) = validate_skill_name(&entry.id) {
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(error));
            }
            if seen.insert(entry.id.clone(), ()).is_some() {
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(format!(
                    "技能包中存在重复 Skill '{}'",
                    entry.id
                )));
            }
            let source = extract_dir.join("skills").join(&entry.id);
            match fs::symlink_metadata(&source) {
                Ok(metadata) if metadata.file_type().is_dir() => {}
                _ => {
                    let _ = fs::remove_dir_all(&temp);
                    return Err(SkillBundleImportError::invalid(format!(
                        "技能包缺少 Skill '{}' 的目录",
                        entry.id
                    )));
                }
            }
            if let Err(error) = validate_bundle_skill_source(&source) {
                let _ = fs::remove_dir_all(&temp);
                return Err(error);
            }
            plans.push(BundleImportPlan {
                id: entry.id.clone(),
                role: entry.role,
                source,
            });
        }
        if let Err(error) = validate_bundle_layout(&extract_dir, &seen) {
            let _ = fs::remove_dir_all(&temp);
            return Err(error);
        }

        let conflicts: Vec<String> = plans
            .iter()
            .filter(|plan| self.find_skill(&plan.id).is_some())
            .map(|plan| plan.id.clone())
            .collect();
        if !replace_existing && !conflicts.is_empty() {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError {
                message: format!(
                    "{} 个同名 Skill 已存在，确认后会覆盖其文件和角色",
                    conflicts.len()
                ),
                conflicts,
            });
        }

        if let Err(error) = fs::create_dir_all(&install_root) {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid(format!(
                "无法创建 Skill 目录: {}",
                error
            )));
        }
        let parent = install_root
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| PathBuf::from("."));
        let staging_root = parent.join(format!(".qzdesk-bundle-stage-{}", token));
        let backup_root = parent.join(format!(".qzdesk-bundle-backup-{}", token));
        let _ = fs::remove_dir_all(&staging_root);
        let _ = fs::remove_dir_all(&backup_root);
        if let Err(error) = fs::create_dir_all(&staging_root) {
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid(format!(
                "无法创建导入暂存目录: {}",
                error
            )));
        }
        if let Err(error) = fs::create_dir_all(&backup_root) {
            let _ = fs::remove_dir_all(&staging_root);
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid(format!(
                "无法创建导入备份目录: {}",
                error
            )));
        }

        for plan in &plans {
            let target = staging_root.join(&plan.id);
            if let Err(error) = copy_bundle_skill(&plan.source, &target) {
                let _ = fs::remove_dir_all(&staging_root);
                let _ = fs::remove_dir_all(&backup_root);
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(format!(
                    "准备 Skill '{}' 失败: {}",
                    plan.id, error
                )));
            }
            if let Err(error) = validate_skill_file(&target.join("SKILL.md")) {
                let _ = fs::remove_dir_all(&staging_root);
                let _ = fs::remove_dir_all(&backup_root);
                let _ = fs::remove_dir_all(&temp);
                return Err(SkillBundleImportError::invalid(error));
            }
        }

        // OEM / 源码目录里的同名项也会在确认后被用户版本覆盖，计数要把它们
        // 算进去，界面显示的“替换数量”才与冲突提示一致。
        let replaced = conflicts.len();
        let mut committed = Vec::with_capacity(plans.len());
        let mut backups = Vec::new();
        let mut commit_error = None;
        for plan in &plans {
            let target = install_root.join(&plan.id);
            let backup = backup_root.join(&plan.id);
            if path_exists(&target) {
                if let Err(error) = fs::rename(&target, &backup) {
                    commit_error = Some(format!("备份旧 Skill '{}' 失败: {}", plan.id, error));
                    break;
                }
                backups.push(plan.id.clone());
            }
            if let Err(error) = fs::rename(staging_root.join(&plan.id), &target) {
                commit_error = Some(format!("安装 Skill '{}' 失败: {}", plan.id, error));
                break;
            }
            committed.push(plan.id.clone());
        }
        if let Some(error) = commit_error {
            rollback_bundle_commit(&install_root, &backup_root, &committed, &backups);
            let _ = fs::remove_dir_all(&staging_root);
            let _ = fs::remove_dir_all(&backup_root);
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid(error));
        }

        let mut state = self.load_state();
        for plan in &plans {
            state
                .skills
                .insert(plan.id.clone(), SkillSelection { role: plan.role });
        }
        if let Err(error) = self.save_state(&state) {
            rollback_bundle_commit(&install_root, &backup_root, &committed, &backups);
            let _ = fs::remove_dir_all(&staging_root);
            let _ = fs::remove_dir_all(&backup_root);
            let _ = fs::remove_dir_all(&temp);
            return Err(SkillBundleImportError::invalid(error));
        }
        if let Err(error) = sync_directory(&install_root) {
            log::warn!("技能包已导入，但同步安装目录失败: {}", error);
        }

        let _ = fs::remove_dir_all(&staging_root);
        let _ = fs::remove_dir_all(&backup_root);
        let _ = fs::remove_dir_all(&temp);
        let primary = plans
            .iter()
            .filter(|plan| plan.role == SkillRole::Primary)
            .count();
        let secondary = plans
            .iter()
            .filter(|plan| plan.role == SkillRole::Secondary)
            .count();
        let disabled = plans
            .iter()
            .filter(|plan| plan.role == SkillRole::None)
            .count();
        log::info!(
            "Imported {} skills from bundle ({} replaced)",
            plans.len(),
            replaced
        );
        Ok(json!({
            "imported": plans.len(),
            "replaced": replaced,
            "primary": primary,
            "secondary": secondary,
            "disabled": disabled
        }))
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
                            log::warn!(
                                "主技能 {} 的 SKILL.md 读取失败，本次未注入：{}",
                                dir_name,
                                error
                            )
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

        // 主技能正文共用一个预算池：先给规则和备用目录留 3000 字符，剩下的按主
        // 技能数量平分，避免靠后的主技能被整体截掉
        let pool = MAX_INSTRUCTIONS_CHARS.saturating_sub(3_000);
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
        sections.insert(
            0,
            format!(
                "{}\n{}\n{}\n{}",
                crate::companion::INSTRUCTIONS,
                companion_style_rules(),
                crate::companion::memory_instructions(),
                crate::companion::light_memory_instructions()
            ),
        );
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
        let dir = self
            .active_skill_dirs()
            .into_iter()
            .find(|(candidate, _)| candidate == skill)
            .map(|(_, path)| path)
            .ok_or_else(|| format!("Skill '{}' 未启用或不存在", skill))?;
        let relative = relative_path.unwrap_or("SKILL.md");
        let relative_path = Path::new(relative);
        if relative_path.is_absolute()
            || relative_path
                .components()
                .any(|component| matches!(component, std::path::Component::ParentDir))
        {
            return Err("Invalid Skill path".to_string());
        }
        let path = dir.join(relative_path);
        // Reject symlinks that escape the Skill directory, in addition to the
        // lexical `..` check above.
        let canonical_dir =
            fs::canonicalize(&dir).map_err(|e| format!("Cannot access Skill directory: {}", e))?;
        let canonical_path =
            fs::canonicalize(&path).map_err(|e| format!("Cannot read Skill file: {}", e))?;
        if !canonical_path.starts_with(&canonical_dir) {
            return Err("Skill path escapes its directory".to_string());
        }
        let metadata =
            fs::metadata(&canonical_path).map_err(|e| format!("Cannot read Skill file: {}", e))?;
        if !metadata.is_file() || metadata.len() > MAX_FILE_BYTES {
            return Err("Skill file is missing or too large".to_string());
        }
        let content = fs::read_to_string(&canonical_path)
            .map_err(|e| format!("Cannot decode Skill file as UTF-8: {}", e))?;

        let total_lines = content.lines().count();
        let start = start_line.unwrap_or(1).max(1);
        let limit = max_chars
            .unwrap_or(MAX_READ_CHARS)
            .clamp(200, MAX_READ_CHARS * 4);

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
                    let (score, matched) = Self::score_chunk(&query_lower, &terms, chunk, &names);
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
                if scanned.is_empty() {
                    "（无）".to_string()
                } else {
                    scanned.join(" / ")
                },
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
            } else if meta.file_type().is_file()
                && path.file_name().and_then(|v| v.to_str()) == Some("SKILL.md")
            {
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
    let content =
        fs::read_to_string(path).map_err(|e| format!("SKILL.md 不是有效的 UTF-8 文件: {}", e))?;
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
    if !name
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-' || b == b'.')
    {
        return Err("Skill 名称只能包含字母、数字、下划线、短横线和点".to_string());
    }
    Ok(())
}

fn is_bundle_cache_component(name: &str) -> bool {
    name.starts_with('.')
        || matches!(
            name,
            "__pycache__" | "node_modules" | "target" | "Thumbs.db"
        )
}

fn collect_bundle_files(root: &Path) -> Result<Vec<BundleFile>, String> {
    let mut files = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let entries = fs::read_dir(&dir)
            .map_err(|error| format!("无法读取目录 '{}': {}", dir.display(), error))?;
        for entry in entries {
            let entry =
                entry.map_err(|error| format!("无法读取目录 '{}': {}", dir.display(), error))?;
            let name = entry.file_name();
            let name_text = name
                .to_str()
                .ok_or_else(|| format!("文件路径不是有效 UTF-8: {}", entry.path().display()))?;
            if is_bundle_cache_component(name_text) {
                continue;
            }
            let path = entry.path();
            let metadata = fs::symlink_metadata(&path)
                .map_err(|error| format!("无法读取 '{}': {}", path.display(), error))?;
            if metadata.file_type().is_dir() {
                stack.push(path);
            } else if metadata.file_type().is_file() {
                let relative = path
                    .strip_prefix(root)
                    .map_err(|_| format!("无法计算文件路径: {}", path.display()))?;
                let mut parts = Vec::new();
                for component in relative.components() {
                    let value = component
                        .as_os_str()
                        .to_str()
                        .ok_or_else(|| format!("文件路径不是有效 UTF-8: {}", path.display()))?;
                    parts.push(value);
                }
                files.push(BundleFile {
                    relative: parts.join("/"),
                    path,
                    size: metadata.len(),
                });
            }
        }
    }
    files.sort_by(|left, right| left.relative.cmp(&right.relative));
    Ok(files)
}

fn validate_bundle_skill_source(source: &Path) -> Result<(), SkillBundleImportError> {
    validate_skill_file(&source.join("SKILL.md")).map_err(SkillBundleImportError::invalid)?;
    let files = collect_bundle_files(source).map_err(SkillBundleImportError::invalid)?;
    if !files.iter().any(|file| file.relative == "SKILL.md") {
        return Err(SkillBundleImportError::invalid(
            "技能包中的 Skill 缺少 SKILL.md",
        ));
    }
    let bytes = files
        .iter()
        .try_fold(0u64, |total, file| total.checked_add(file.size))
        .ok_or_else(|| SkillBundleImportError::invalid("Skill 体积超出限制"))?;
    if let Some((max_bytes, max_files)) = unpacked_limits() {
        if bytes > max_bytes || files.len() > max_files {
            return Err(SkillBundleImportError::invalid(format!(
                "单个 Skill 解压后过大（{} KB / {} 个文件），上限 {} MB / {} 个文件",
                bytes / 1024,
                files.len(),
                max_bytes / 1024 / 1024,
                max_files
            )));
        }
    }
    Ok(())
}

/// 技能包是版本化格式，目录结构必须严格匹配清单。这样拼错清单、夹带
/// 未声明目录或在 `skills/` 下放普通文件都会在提交前得到明确错误。
fn validate_bundle_layout(
    root: &Path,
    expected: &BTreeMap<String, ()>,
) -> Result<(), SkillBundleImportError> {
    let mut manifest_found = false;
    let mut skills_dir = None;
    let entries = fs::read_dir(root).map_err(|error| {
        SkillBundleImportError::invalid(format!("无法读取技能包目录: {}", error))
    })?;
    for entry in entries {
        let entry = entry.map_err(|error| {
            SkillBundleImportError::invalid(format!("无法读取技能包目录: {}", error))
        })?;
        let name = entry.file_name();
        let name = name
            .to_str()
            .ok_or_else(|| SkillBundleImportError::invalid("技能包根目录含有非 UTF-8 路径"))?;
        let file_type = fs::symlink_metadata(entry.path())
            .map_err(|error| {
                SkillBundleImportError::invalid(format!(
                    "无法检查技能包路径 '{}': {}",
                    entry.path().display(),
                    error
                ))
            })?
            .file_type();
        if name == BUNDLE_MANIFEST && file_type.is_file() {
            manifest_found = true;
        } else if name == "skills" && file_type.is_dir() {
            skills_dir = Some(entry.path());
        } else {
            return Err(SkillBundleImportError::invalid(format!(
                "技能包根目录含有未支持的内容: {}",
                name
            )));
        }
    }
    if !manifest_found {
        return Err(SkillBundleImportError::invalid(
            "技能包缺少 qzdesk-skills.json 清单",
        ));
    }
    let skills_dir =
        skills_dir.ok_or_else(|| SkillBundleImportError::invalid("技能包缺少 skills 目录"))?;

    let mut found = BTreeMap::new();
    let entries = fs::read_dir(&skills_dir)
        .map_err(|error| SkillBundleImportError::invalid(format!("无法读取技能目录: {}", error)))?;
    for entry in entries {
        let entry = entry.map_err(|error| {
            SkillBundleImportError::invalid(format!("无法读取技能目录: {}", error))
        })?;
        let name = entry.file_name();
        let name = name
            .to_str()
            .ok_or_else(|| SkillBundleImportError::invalid("技能目录含有非 UTF-8 路径"))?;
        if !expected.contains_key(name) {
            return Err(SkillBundleImportError::invalid(format!(
                "技能包含有清单未声明的目录: {}",
                name
            )));
        }
        let metadata = fs::symlink_metadata(entry.path()).map_err(|error| {
            SkillBundleImportError::invalid(format!("无法检查 Skill '{}': {}", name, error))
        })?;
        if !metadata.file_type().is_dir() {
            return Err(SkillBundleImportError::invalid(format!(
                "Skill '{}' 不是目录",
                name
            )));
        }
        if found.insert(name.to_string(), ()).is_some() {
            return Err(SkillBundleImportError::invalid(format!(
                "技能包中存在重复 Skill '{}'",
                name
            )));
        }
    }
    if found.len() != expected.len() {
        let missing = expected
            .keys()
            .find(|name| !found.contains_key(*name))
            .cloned()
            .unwrap_or_default();
        return Err(SkillBundleImportError::invalid(format!(
            "技能包缺少 Skill '{}' 的目录",
            missing
        )));
    }
    Ok(())
}

fn copy_bundle_skill(source: &Path, target: &Path) -> std::io::Result<()> {
    let files = collect_bundle_files(source)
        .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error))?;
    fs::create_dir_all(target)?;
    for file in files {
        let output = target.join(&file.relative);
        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::copy(&file.path, &output)?;
    }
    Ok(())
}

fn path_exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

fn remove_path(path: &Path) {
    match fs::symlink_metadata(path) {
        Ok(metadata) if metadata.file_type().is_dir() => {
            let _ = fs::remove_dir_all(path);
        }
        Ok(_) => {
            let _ = fs::remove_file(path);
        }
        Err(_) => {}
    }
}

fn rollback_bundle_commit(
    install_root: &Path,
    backup_root: &Path,
    committed: &[String],
    backups: &[String],
) {
    for id in committed.iter().rev() {
        remove_path(&install_root.join(id));
    }
    for id in backups.iter().rev() {
        let _ = fs::rename(backup_root.join(id), install_root.join(id));
    }
}

fn zip_bytes(data: &[u8], offset: usize, len: usize) -> Result<&[u8], String> {
    let end = offset
        .checked_add(len)
        .ok_or_else(|| "ZIP 结构超出文件边界".to_string())?;
    data.get(offset..end)
        .ok_or_else(|| "ZIP 结构超出文件边界".to_string())
}

fn zip_u16(data: &[u8], offset: usize) -> Result<u16, String> {
    let bytes = zip_bytes(data, offset, 2)?;
    Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
}

fn zip_u32(data: &[u8], offset: usize) -> Result<u32, String> {
    let bytes = zip_bytes(data, offset, 4)?;
    Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

/// 在启动外部 `unzip` 前读取中央目录。嵌入式系统常只有 BusyBox
/// `unzip`，它没有 Info-ZIP 的 `-Z1/-l` 参数；中央目录则能直接给出
/// 条目名、声明体积和 Unix 文件类型，足够做解压前安全检查。
fn validate_bundle_zip(data: &[u8]) -> Result<(), String> {
    const EOCD_LEN: usize = 22;
    if data.len() < EOCD_LEN {
        return Err("ZIP 文件过短，无法读取目录".to_string());
    }

    let search_floor = data.len().saturating_sub(EOCD_LEN + u16::MAX as usize);
    let mut eocd = None;
    for offset in (search_floor..=data.len() - EOCD_LEN).rev() {
        if zip_bytes(data, offset, 4)? != b"PK\x05\x06" {
            continue;
        }
        let comment_len = zip_u16(data, offset + 20)? as usize;
        let record_end = offset
            .checked_add(EOCD_LEN)
            .and_then(|value| value.checked_add(comment_len));
        if record_end == Some(data.len()) {
            eocd = Some(offset);
            break;
        }
    }
    let eocd = eocd.ok_or_else(|| "ZIP 文件缺少结束目录".to_string())?;

    let disk = zip_u16(data, eocd + 4)?;
    let central_disk = zip_u16(data, eocd + 6)?;
    let disk_entries = zip_u16(data, eocd + 8)?;
    let total_entries = zip_u16(data, eocd + 10)?;
    let central_size = zip_u32(data, eocd + 12)?;
    let central_offset = zip_u32(data, eocd + 16)?;
    if total_entries == u16::MAX
        || disk_entries == u16::MAX
        || central_size == u32::MAX
        || central_offset == u32::MAX
    {
        return Err("暂不支持 ZIP64 技能包".to_string());
    }
    if disk != 0 || central_disk != 0 || disk_entries != total_entries {
        return Err("不支持分卷 ZIP 技能包".to_string());
    }
    if total_entries == 0 {
        return Err("ZIP 文件为空".to_string());
    }
    if total_entries as usize > MAX_BUNDLE_FILES {
        return Err(format!("技能包文件数超过 {} 个上限", MAX_BUNDLE_FILES));
    }

    let central_start = central_offset as usize;
    let central_end = central_start
        .checked_add(central_size as usize)
        .ok_or_else(|| "ZIP 中央目录超出文件边界".to_string())?;
    if central_end > eocd {
        return Err("ZIP 中央目录超出文件边界".to_string());
    }

    let mut cursor = central_start;
    let mut seen = BTreeMap::new();
    let mut declared_bytes = 0u64;
    for _ in 0..total_entries {
        if zip_bytes(data, cursor, 4)? != b"PK\x01\x02" {
            return Err("ZIP 中央目录条目标记错误".to_string());
        }
        zip_bytes(data, cursor, 46).map_err(|_| "ZIP 中央目录条目被截断".to_string())?;
        let flags = zip_u16(data, cursor + 8)?;
        if flags & 0x0041 != 0 {
            return Err("暂不支持加密 ZIP 技能包".to_string());
        }
        let compressed_size = zip_u32(data, cursor + 20)?;
        let uncompressed_size = zip_u32(data, cursor + 24)?;
        let name_len = zip_u16(data, cursor + 28)? as usize;
        let extra_len = zip_u16(data, cursor + 30)? as usize;
        let comment_len = zip_u16(data, cursor + 32)? as usize;
        let entry_disk = zip_u16(data, cursor + 34)?;
        let external_attrs = zip_u32(data, cursor + 38)?;
        let local_offset = zip_u32(data, cursor + 42)?;
        if compressed_size == u32::MAX
            || uncompressed_size == u32::MAX
            || entry_disk == u16::MAX
            || local_offset == u32::MAX
        {
            return Err("暂不支持 ZIP64 技能包".to_string());
        }
        if entry_disk != 0 {
            return Err("不支持分卷 ZIP 技能包".to_string());
        }

        let name_start = cursor
            .checked_add(46)
            .ok_or_else(|| "ZIP 条目名超出文件边界".to_string())?;
        let name_bytes = zip_bytes(data, name_start, name_len)?;
        let name = std::str::from_utf8(name_bytes)
            .map_err(|_| "技能包路径不是有效的 UTF-8".to_string())?;
        validate_bundle_zip_path(name, &mut seen)?;

        let unix_type = (external_attrs >> 16) & 0o170000;
        if unix_type == 0o120000 {
            return Err(format!("技能包含有不支持的符号链接: {}", name));
        }
        if unix_type != 0 && unix_type != 0o100000 && unix_type != 0o040000 {
            return Err(format!("技能包含有不支持的文件类型: {}", name));
        }

        let local_start = local_offset as usize;
        if local_start >= central_start || zip_bytes(data, local_start, 4)? != b"PK\x03\x04" {
            return Err(format!("Skill ZIP 条目的本地文件头错误: {}", name));
        }
        let local_flags = zip_u16(data, local_start + 6)?;
        if local_flags & 0x0041 != 0 {
            return Err("暂不支持加密 ZIP 技能包".to_string());
        }
        let local_name_len = zip_u16(data, local_start + 26)? as usize;
        let local_extra_len = zip_u16(data, local_start + 28)? as usize;
        let local_name_start = local_start
            .checked_add(30)
            .ok_or_else(|| "ZIP 本地文件头超出边界".to_string())?;
        if zip_bytes(data, local_name_start, local_name_len)? != name_bytes {
            return Err(format!(
                "ZIP 条目名在中央目录与本地文件头中不一致: {}",
                name
            ));
        }
        let data_start = local_name_start
            .checked_add(local_name_len)
            .and_then(|value| value.checked_add(local_extra_len))
            .ok_or_else(|| "ZIP 数据偏移超出边界".to_string())?;
        let data_end = data_start
            .checked_add(compressed_size as usize)
            .ok_or_else(|| "ZIP 数据长度超出边界".to_string())?;
        if data_end > central_start {
            return Err(format!("ZIP 条目数据超出边界: {}", name));
        }

        declared_bytes = declared_bytes
            .checked_add(uncompressed_size as u64)
            .ok_or_else(|| "技能包声明的解压体积超出限制".to_string())?;
        if declared_bytes > MAX_BUNDLE_CONTENT_BYTES {
            return Err(format!(
                "技能包解压后约 {} MiB，超过 {} MiB 上限",
                declared_bytes / 1024 / 1024,
                MAX_BUNDLE_CONTENT_BYTES / 1024 / 1024
            ));
        }
        cursor = name_start
            .checked_add(name_len)
            .and_then(|value| value.checked_add(extra_len))
            .and_then(|value| value.checked_add(comment_len))
            .ok_or_else(|| "ZIP 中央目录条目超出边界".to_string())?;
    }
    if cursor != central_end {
        return Err("ZIP 中央目录含有未支持的内容".to_string());
    }
    Ok(())
}

fn validate_bundle_zip_path(name: &str, seen: &mut BTreeMap<String, ()>) -> Result<(), String> {
    if name.starts_with('/') || name.starts_with('\\') || name.contains('\\') {
        return Err(format!("技能包含有不安全路径: {}", name));
    }
    if name.chars().any(char::is_control) {
        return Err("技能包含有控制字符路径".to_string());
    }
    let path = name.trim_end_matches('/');
    if path.is_empty() {
        return Err("技能包含有空路径".to_string());
    }
    for component in path.split('/') {
        if component.is_empty() || component == "." || component == ".." {
            return Err(format!("技能包含有路径穿越: {}", name));
        }
    }
    if seen.insert(path.to_string(), ()).is_some() {
        return Err(format!("技能包含有重复路径: {}", name));
    }
    Ok(())
}

fn validate_extracted_bundle_tree(root: &Path) -> Result<(), SkillBundleImportError> {
    let canonical_root = fs::canonicalize(root)
        .map_err(|error| SkillBundleImportError::invalid(format!("无法检查解压目录: {}", error)))?;
    let mut total_bytes = 0u64;
    let mut total_files = 0usize;
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let entries = fs::read_dir(&dir).map_err(|error| {
            SkillBundleImportError::invalid(format!(
                "无法读取解压目录 '{}': {}",
                dir.display(),
                error
            ))
        })?;
        for entry in entries {
            let entry = entry.map_err(|error| {
                SkillBundleImportError::invalid(format!(
                    "无法读取解压目录 '{}': {}",
                    dir.display(),
                    error
                ))
            })?;
            let path = entry.path();
            let metadata = fs::symlink_metadata(&path).map_err(|error| {
                SkillBundleImportError::invalid(format!("无法读取 '{}': {}", path.display(), error))
            })?;
            let file_type = metadata.file_type();
            if file_type.is_symlink() {
                return Err(SkillBundleImportError::invalid(format!(
                    "技能包含有不支持的符号链接: {}",
                    path.display()
                )));
            }
            if file_type.is_dir() {
                let canonical = fs::canonicalize(&path).map_err(|error| {
                    SkillBundleImportError::invalid(format!(
                        "无法检查 '{}': {}",
                        path.display(),
                        error
                    ))
                })?;
                if !canonical.starts_with(&canonical_root) {
                    return Err(SkillBundleImportError::invalid("技能包含有越界目录"));
                }
                stack.push(path);
            } else if file_type.is_file() {
                let canonical = fs::canonicalize(&path).map_err(|error| {
                    SkillBundleImportError::invalid(format!(
                        "无法检查 '{}': {}",
                        path.display(),
                        error
                    ))
                })?;
                if !canonical.starts_with(&canonical_root) {
                    return Err(SkillBundleImportError::invalid("技能包含有越界文件"));
                }
                total_bytes = total_bytes
                    .checked_add(metadata.len())
                    .ok_or_else(|| SkillBundleImportError::invalid("技能包体积超出限制"))?;
                total_files += 1;
            } else {
                return Err(SkillBundleImportError::invalid(format!(
                    "技能包含有不支持的文件类型: {}",
                    path.display()
                )));
            }
        }
    }
    if total_bytes > MAX_BUNDLE_CONTENT_BYTES {
        return Err(SkillBundleImportError::invalid(format!(
            "技能包解压后超过 {} MiB 上限",
            MAX_BUNDLE_CONTENT_BYTES / 1024 / 1024
        )));
    }
    if total_files > MAX_BUNDLE_FILES {
        return Err(SkillBundleImportError::invalid(format!(
            "技能包文件数超过 {} 个上限",
            MAX_BUNDLE_FILES
        )));
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
        Err(_) => {
            return SkillMetadata {
                name: None,
                description: None,
            };
        }
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
    pub fn new(manager: SkillManager) -> Self {
        Self { manager }
    }
}

#[async_trait]
impl McpTool for SkillListTool {
    fn name(&self) -> &str {
        "skill_list"
    }
    fn description(&self) -> &str {
        "列出本会话可用的本地 Skill：主技能（正文已随会话注入，必须执行）和备用技能（按需用 skill_search / skill_read 查阅）。用户在设备上关闭的 Skill 不会出现在这里。"
    }
    fn input_schema(&self) -> Value {
        json!({ "type": "object", "properties": {} })
    }
    async fn call(&self, _params: Value) -> Result<Value, String> {
        Ok(self.manager.list_for_model())
    }
}

pub struct SkillSearchTool {
    manager: SkillManager,
}

impl SkillSearchTool {
    pub fn new(manager: SkillManager) -> Self {
        Self { manager }
    }
}

#[async_trait]
impl McpTool for SkillSearchTool {
    fn name(&self) -> &str {
        "skill_search"
    }
    fn description(&self) -> &str {
        "按用户请求检索本地 Skill 的具体章节。主技能正文已在会话里，只有需要参考文件、或技能正文被截断的后半部分时才检索；备用技能在被明确需要时检索。返回结果按 Markdown 章节切分，包含 section（章节标题）、path（文件）与 line_start（起始行号），命中后用 skill_read 读取原文。"
    }
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
        let query = params
            .get("query")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let skill = params.get("skill").and_then(Value::as_str);
        let limit = params
            .get("limit")
            .and_then(Value::as_u64)
            .unwrap_or(MAX_SEARCH_RESULTS as u64);
        self.manager.search(skill, query, limit as usize)
    }
}

pub struct SkillReadTool {
    manager: SkillManager,
}

impl SkillReadTool {
    pub fn new(manager: SkillManager) -> Self {
        Self { manager }
    }
}

#[async_trait]
impl McpTool for SkillReadTool {
    fn name(&self) -> &str {
        "skill_read"
    }
    fn description(&self) -> &str {
        "读取本地 Skill 的 SKILL.md 或检索结果指定的参考文件。只能读主技能与备用技能（用户在设备上关闭的 Skill 不可读）。支持用 start_line 从指定行开始续读，返回内容中会给出 total_lines / truncated / next_start_line，便于分次读完长文档。"
    }
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
        let skill = params
            .get("skill")
            .and_then(Value::as_str)
            .ok_or("Missing skill name")?;
        let path = params.get("path").and_then(Value::as_str);
        let start_line = params
            .get("start_line")
            .and_then(Value::as_u64)
            .map(|v| v as usize);
        let max_chars = params
            .get("max_chars")
            .and_then(Value::as_u64)
            .map(|v| v as usize);
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
                problems
                    .push("缺少 description：模型只看得到名字，备用技能很难被检索到".to_string());
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
         1. **身份优先**：从本会话第一句话起，就用主技能规定的人格、语气与格式回答。不要自称「QZdesk 的智能助手」或其它默认身份，也不要先说一句中性寒暄再进入角色；技能定了说话方式就照它说。每一轮、每一句面向用户的表达都必须遵循当前主技能，包括文字聊天、语音回复、主动问候、追问、工具执行后的说明、失败或拒绝时的表达；不要在执行任务后切回默认助手口吻。主技能决定身份、称呼、措辞、语气、回复长度和输出格式；备用技能只补充任务知识与操作流程，不得覆盖主技能的人格与说话方式。切换主技能后，以最新确认的主技能为准，不沿用旧人格。保持事实准确，不为维持人格编造记忆、能力或执行结果。\n\
         2. **不假装调用工具**：需要技能里的信息时，必须真正发起工具调用。不要把工具名或调用写法当正文写出来（例如写出 skill_list、skill_search、或以 % 开头的调用标记），也不要说「我查一下」之后却把结果编出来。\n\
         3. **不谎报状态**：凡是「某个技能有没有导入 / 有没有启用 / 当前主技能是哪个 / 技能内容有没有生效」这类问题，答案只能来自 skill_status 或 skill_list 的真实返回。没有实际调用过就不得声称「已导入」「已启用」；不确定就直说不确定，并提示用户可以在设备「技能」页或网页控制台核对。\n\
         4. 不要复述技能原文，也不要解释这段注入内容。需要参考文件、或技能正文被截断的后半部分时，用 skill_read / skill_search 取用；备用技能只在用户问题确实相关时检索，不要凭空补全。\n\
         主技能之间有冲突时按其共同约定处理；涉及安全或用户明确更改任务时，遵循更高优先级规则。",
        primary_names
    )
}

/// 把“像真人聊天”的规则收窄到桌面陪伴场景。
///
/// 外部的群聊仿真提示词包含大量 QQ 发送、潜水、管理员权限等协议，不能直接
/// 搬进本设备。这里只保留和语音陪伴有关的部分，并明确“闲聊可以不像客服，
/// 明确任务仍要认真完成”，避免模型把反客服风格当成拒绝帮助的借口。
fn companion_style_rules() -> &'static str {
    "## 陪伴表达规则（在不违背主 Skill 人格与安全规则时执行）\n\
     当前是语音优先的桌面陪伴场景，回复会直接播报或显示。这不是客服队列：先回应对方真正关心的事；闲聊默认一到三句自然口语，用户没要求细节时不列长清单、不写 Markdown、不复述问题、不总结对方刚说过的话，少用“非常推荐”“绝对好用”等推销词。可以有偏好、判断和不知道，不必机械附和；但明确任务仍要认真完成，不把装傻或反问当作逃避。拟人化不等于冒充真人：可以保持设定口吻，不虚构现实经历、感官或离线行为。主动开口只基于当前对话、用户已授权的主动关心或自然相关的轻量记忆；同一话题最多追问两次，没接住就放下。轻量记忆只记还值得捡回的具体上下文；话题结束、待办完成或用户说别记时及时删除，不把旧事硬塞进当前对话。工具调用是执行动作，不向用户复述工具名、参数或内部流程；只有真实返回才算完成，失败或未确认时如实说明。不要假装看过、在场或记得。"
}

#[cfg(test)]
mod tests {
    #[test]
    fn injected_rules_demand_persona_and_forbid_fabricating() {
        let header = super::skill_rules_header("`HQQ`");
        // 主技能名字要出现，模型才知道它是谁
        assert!(header.contains("`HQQ`"));
        for needle in [
            "身份优先",
            "QZdesk 的智能助手",
            "不假装调用工具",
            "不谎报状态",
        ] {
            assert!(header.contains(needle), "规则里缺少「{}」", needle);
        }
        // 「已启用/已导入」必须要有工具结果支撑，这句话不能被改掉
        assert!(header.contains("skill_status"));
        assert!(header.contains("不得声称"));
        for needle in [
            "每一轮、每一句",
            "语音回复",
            "主动问候",
            "工具执行后的说明",
            "备用技能只补充",
            "不得覆盖主技能",
            "最新确认的主技能",
        ] {
            assert!(header.contains(needle), "人格覆盖规则缺少：{}", needle);
        }
    }

    #[test]
    fn companion_style_rules_fit_voice_first_and_keep_tasks_useful() {
        let rules = super::companion_style_rules();
        for needle in [
            "语音优先",
            "客服队列",
            "明确任务仍要认真完成",
            "不虚构现实经历",
            "主动关心",
            "轻量记忆",
            "真实返回",
        ] {
            assert!(rules.contains(needle), "陪伴规则缺少：{}", needle);
        }
        for rejected in ["qq_", "mark_read", "ownerQQ", "群管理"] {
            assert!(
                !rules.contains(rejected),
                "桌面陪伴规则不应包含群聊机器人协议：{}",
                rejected
            );
        }
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
        assert!(
            terms.contains(&"查询天气预报".to_string()),
            "整句本身就是一个词"
        );
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

    fn bundle_test_dir(label: &str) -> PathBuf {
        env::temp_dir().join(format!(
            "qzdesk-{}-{}",
            label,
            uuid::Uuid::new_v4().simple()
        ))
    }

    fn write_bundle_test_skill(root: &Path, id: &str, content: &str) {
        let dir = root.join(id);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("SKILL.md"), content).unwrap();
    }

    #[tokio::test]
    async fn bundle_export_round_trips_files_and_roles() {
        let source = bundle_test_dir("bundle-source");
        let target = bundle_test_dir("bundle-target");
        write_bundle_test_skill(&source, "alpha", "# Alpha\nprimary body\n");
        write_bundle_test_skill(&source, "beta", "# Beta\nsecondary body\n");
        write_bundle_test_skill(&source, "gamma", "# Gamma\nclosed body\n");
        fs::create_dir_all(source.join("alpha/references/deep")).unwrap();
        fs::write(
            source.join("alpha/references/deep/guide.md"),
            "keep this reference\n",
        )
        .unwrap();
        fs::create_dir_all(source.join("alpha/__pycache__")).unwrap();
        fs::write(source.join("alpha/__pycache__/ignored.pyc"), "cache").unwrap();
        fs::create_dir_all(source.join("alpha/node_modules/pkg")).unwrap();
        fs::write(source.join("alpha/node_modules/pkg/ignored.js"), "cache").unwrap();

        let source_manager = SkillManager::for_test_dir(source.clone());
        source_manager
            .set_role("alpha", SkillRole::Primary)
            .unwrap();
        source_manager
            .set_role("beta", SkillRole::Secondary)
            .unwrap();
        source_manager.set_role("gamma", SkillRole::None).unwrap();
        let bundle = source_manager.export_bundle().unwrap();
        assert_eq!(&bundle[..4], b"PK\x03\x04");

        let archive = source.with_extension("zip");
        fs::write(&archive, &bundle).unwrap();
        let test_status = Command::new("unzip")
            .arg("-t")
            .arg(&archive)
            .status()
            .await
            .unwrap();
        assert!(test_status.success(), "导出包应通过 unzip -t");

        let target_manager = SkillManager::for_test_dir(target.clone());
        let result = target_manager.import_bundle(&bundle, false).await.unwrap();
        assert_eq!(result["imported"], 3);
        assert_eq!(result["replaced"], 0);
        assert_eq!(result["primary"], 1);
        assert_eq!(result["secondary"], 1);
        assert_eq!(result["disabled"], 1);
        assert_eq!(
            fs::read_to_string(target.join("alpha/SKILL.md")).unwrap(),
            "# Alpha\nprimary body\n"
        );
        assert_eq!(
            fs::read_to_string(target.join("alpha/references/deep/guide.md")).unwrap(),
            "keep this reference\n"
        );
        assert!(!target.join("alpha/__pycache__").exists());
        assert!(!target.join("alpha/node_modules").exists());
        let state = target_manager.load_state();
        assert_eq!(
            state.skills.get("alpha").map(|selection| selection.role),
            Some(SkillRole::Primary)
        );
        assert_eq!(
            state.skills.get("beta").map(|selection| selection.role),
            Some(SkillRole::Secondary)
        );
        assert_eq!(
            state.skills.get("gamma").map(|selection| selection.role),
            Some(SkillRole::None)
        );

        let _ = fs::remove_dir_all(&source);
        let _ = fs::remove_dir_all(&target);
        let _ = fs::remove_file(&archive);
    }

    #[tokio::test]
    async fn bundle_import_requires_confirmation_before_replacing() {
        let source = bundle_test_dir("bundle-conflict-source");
        let target = bundle_test_dir("bundle-conflict-target");
        write_bundle_test_skill(&source, "alpha", "# Alpha\nnew body\n");
        fs::create_dir_all(source.join("alpha/references")).unwrap();
        fs::write(source.join("alpha/references/new.md"), "new reference\n").unwrap();
        let source_manager = SkillManager::for_test_dir(source.clone());
        source_manager
            .set_role("alpha", SkillRole::Primary)
            .unwrap();
        let bundle = source_manager.export_bundle().unwrap();

        write_bundle_test_skill(&target, "alpha", "# Alpha\nold body\n");
        fs::write(target.join("alpha/obsolete.md"), "stale\n").unwrap();
        let target_manager = SkillManager::for_test_dir(target.clone());
        target_manager
            .set_role("alpha", SkillRole::Secondary)
            .unwrap();

        let error = target_manager
            .import_bundle(&bundle, false)
            .await
            .unwrap_err();
        assert_eq!(error.conflicts, vec!["alpha"]);
        assert_eq!(
            fs::read_to_string(target.join("alpha/SKILL.md")).unwrap(),
            "# Alpha\nold body\n"
        );
        assert_eq!(
            target_manager.load_state().skills["alpha"].role,
            SkillRole::Secondary
        );

        let result = target_manager.import_bundle(&bundle, true).await.unwrap();
        assert_eq!(result["replaced"], 1);
        assert_eq!(
            fs::read_to_string(target.join("alpha/SKILL.md")).unwrap(),
            "# Alpha\nnew body\n"
        );
        assert_eq!(
            fs::read_to_string(target.join("alpha/references/new.md")).unwrap(),
            "new reference\n"
        );
        assert!(!target.join("alpha/obsolete.md").exists());
        assert_eq!(
            target_manager.load_state().skills["alpha"].role,
            SkillRole::Primary
        );

        let _ = fs::remove_dir_all(&source);
        let _ = fs::remove_dir_all(&target);
    }

    #[tokio::test]
    async fn bundle_import_rejects_path_traversal_before_extracting() {
        let target = bundle_test_dir("bundle-traversal");
        let escape = env::temp_dir().join(format!(
            "qzdesk-bundle-escape-{}.txt",
            uuid::Uuid::new_v4().simple()
        ));
        let escape_name = escape.file_name().and_then(|name| name.to_str()).unwrap();
        let manifest = json!({
            "format": BUNDLE_FORMAT,
            "version": BUNDLE_VERSION,
            "skills": [{
                "id": "safe",
                "role": "none"
            }]
        });
        let mut archive = StoredZip::new();
        archive
            .add(BUNDLE_MANIFEST, &serde_json::to_vec(&manifest).unwrap())
            .unwrap();
        archive
            .add("skills/safe/SKILL.md", b"# Safe\nnot imported\n")
            .unwrap();
        // StoredZip refuses traversal paths before writing. Build a valid archive
        // first, then rewrite every local/central-directory occurrence to model a
        // hostile archive produced by a different ZIP writer.
        let malicious_name = format!("../../{}", escape_name);
        let placeholder = "x".repeat(malicious_name.len());
        archive.add(&placeholder, b"escaped").unwrap();
        let mut bundle = archive.finish().unwrap();
        let mut cursor = 0;
        let mut replacements = 0;
        while let Some(offset) = bundle[cursor..]
            .windows(placeholder.len())
            .position(|window| window == placeholder.as_bytes())
        {
            let start = cursor + offset;
            bundle[start..start + placeholder.len()].copy_from_slice(malicious_name.as_bytes());
            cursor = start + placeholder.len();
            replacements += 1;
        }
        assert_eq!(replacements, 2, "应同时改写本地与中央目录中的文件名");

        let manager = SkillManager::for_test_dir(target.clone());
        let error = manager.import_bundle(&bundle, false).await.unwrap_err();
        assert!(error.message.contains("路径穿越"), "{}", error.message);
        assert!(error.conflicts.is_empty());
        assert!(!target.join("safe").exists());
        assert!(!escape.exists(), "恶意条目不应写出安装目录");

        let _ = fs::remove_dir_all(&target);
        let _ = fs::remove_file(&escape);
    }

    #[test]
    fn bundle_zip_rejects_unix_symlink_entries_before_extracting() {
        let mut archive = StoredZip::new();
        archive
            .add(BUNDLE_MANIFEST, b"{\"format\":\"qzdesk.skills\"}")
            .unwrap();
        archive
            .add("skills/safe/reference-link", b"/tmp/target")
            .unwrap();
        let mut bundle = archive.finish().unwrap();

        let eocd = bundle
            .windows(4)
            .rposition(|window| window == b"PK\x05\x06")
            .unwrap();
        let central_offset =
            u32::from_le_bytes(bundle[eocd + 16..eocd + 20].try_into().unwrap()) as usize;
        let mut cursor = central_offset;
        loop {
            assert_eq!(&bundle[cursor..cursor + 4], b"PK\x01\x02");
            let name_len =
                u16::from_le_bytes(bundle[cursor + 28..cursor + 30].try_into().unwrap()) as usize;
            let extra_len =
                u16::from_le_bytes(bundle[cursor + 30..cursor + 32].try_into().unwrap()) as usize;
            let comment_len =
                u16::from_le_bytes(bundle[cursor + 32..cursor + 34].try_into().unwrap()) as usize;
            let name_start = cursor + 46;
            let name = &bundle[name_start..name_start + name_len];
            if name == b"skills/safe/reference-link" {
                let symlink_mode = (0o120777u32) << 16;
                bundle[cursor + 38..cursor + 42].copy_from_slice(&symlink_mode.to_le_bytes());
                break;
            }
            cursor = name_start + name_len + extra_len + comment_len;
        }

        let error = validate_bundle_zip(&bundle).unwrap_err();
        assert!(error.contains("符号链接"), "{}", error);
    }
}
