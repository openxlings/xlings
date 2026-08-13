export module xlings.core.i18n;

import std;

import xlings.platform;

namespace xlings::i18n {

// Message IDs for all user-visible strings
export enum class Msg {
    // General
    VERSION_INFO,
    UNKNOWN_COMMAND,
    USE_HELP,
    CONFIRM_YES_NO,

    // Install
    INSTALL_START,
    INSTALL_DONE,
    INSTALL_FAILED,
    INSTALL_SKIPPED_ALREADY,
    INSTALL_NO_PACKAGE,
    INSTALL_CONFIRM,

    // Remove
    REMOVE_START,
    REMOVE_DONE,
    REMOVE_FAILED,
    REMOVE_CONFIRM,

    // Update
    UPDATE_INDEX_START,
    UPDATE_INDEX_DONE,
    UPDATE_REPO_SYNCING,
    UPDATE_REPO_DONE,

    // Search
    SEARCH_NO_RESULTS,
    SEARCH_RESULTS_HEADER,

    // Download
    DOWNLOAD_START,
    DOWNLOAD_PROGRESS,
    DOWNLOAD_DONE,
    DOWNLOAD_FAILED,
    DOWNLOAD_RETRY,

    // Dependencies
    DEPS_RESOLVING,
    DEPS_CYCLE_DETECTED,
    DEPS_CONFLICT,
    DEPS_PLAN_HEADER,

    // Errors
    ERR_PACKAGE_NOT_FOUND,
    ERR_INDEX_LOAD_FAILED,
    ERR_HOOK_FAILED,
    ERR_CHECKSUM_MISMATCH,

    // List
    LIST_HEADER,
    LIST_EMPTY,

    // Info
    INFO_HEADER,

    MSG_COUNT_  // sentinel — must be last
};

constexpr int MSG_COUNT = static_cast<int>(Msg::MSG_COUNT_);

// Bilingual message table: [msg_id][0] = en, [msg_id][1] = zh
// Using std::string_view pairs stored in a flat array
struct MsgEntry {
    std::string_view en;
    std::string_view zh;
};

// clang-format off
constexpr MsgEntry gMessages_[] = {
    // VERSION_INFO
    { "xlings version: {}",                       "xlings 版本: {}" },
    // UNKNOWN_COMMAND
    { "Unknown command: {}",                       "未知命令: {}" },
    // USE_HELP
    { "Use 'xlings help' for usage information",   "使用 'xlings help' 查看帮助" },
    // CONFIRM_YES_NO
    { "{} [y/N] ",                                 "{} [y/N] " },

    // INSTALL_START
    { "Installing {} ...",                         "正在安装 {} ..." },
    // INSTALL_DONE
    { "{} installed successfully",                 "{} 安装成功" },
    // INSTALL_FAILED
    { "Failed to install {}: {}",                  "安装 {} 失败: {}" },
    // INSTALL_SKIPPED_ALREADY
    { "{} is already installed",                   "{} 已安装" },
    // INSTALL_NO_PACKAGE
    { "No package specified",                      "未指定包名" },
    // INSTALL_CONFIRM
    { "Install {} packages?",                      "安装 {} 个包?" },

    // REMOVE_START
    { "Removing {} ...",                           "正在卸载 {} ..." },
    // REMOVE_DONE
    { "{} removed successfully",                   "{} 卸载成功" },
    // REMOVE_FAILED
    { "Failed to remove {}: {}",                   "卸载 {} 失败: {}" },
    // REMOVE_CONFIRM
    { "Remove {}?",                                "卸载 {}?" },

    // UPDATE_INDEX_START
    { "Updating package index ...",                "正在更新包索引 ..." },
    // UPDATE_INDEX_DONE
    { "Package index updated ({} packages)",       "包索引已更新 ({} 个包)" },
    // UPDATE_REPO_SYNCING
    { "Syncing repository: {} ...",                "正在同步仓库: {} ..." },
    // UPDATE_REPO_DONE
    { "Repository synced: {}",                     "仓库已同步: {}" },

    // SEARCH_NO_RESULTS
    { "No packages found for '{}'",                "未找到 '{}' 相关的包" },
    // SEARCH_RESULTS_HEADER
    { "Search results for '{}':",                  "'{}' 的搜索结果:" },

    // DOWNLOAD_START
    { "Downloading {} ...",                        "正在下载 {} ..." },
    // DOWNLOAD_PROGRESS
    { "  {} {}%",                                   "  {} {}%" },
    // DOWNLOAD_DONE
    { "Downloaded {}",                             "已下载 {}" },
    // DOWNLOAD_FAILED
    { "Download failed for {}: {}",                "下载 {} 失败: {}" },
    // DOWNLOAD_RETRY
    { "Retrying download for {} (attempt {})",     "重试下载 {} (第 {} 次)" },

    // DEPS_RESOLVING
    { "Resolving dependencies ...",                "正在解析依赖 ..." },
    // DEPS_CYCLE_DETECTED
    { "Dependency cycle detected: {}",             "检测到循环依赖: {}" },
    // DEPS_CONFLICT
    { "Conflict: {} and {} are mutually exclusive","冲突: {} 和 {} 互斥" },
    // DEPS_PLAN_HEADER
    { "The following packages will be installed:", "将安装以下包:" },

    // ERR_PACKAGE_NOT_FOUND
    { "Package not found: {}",                     "未找到包: {}" },
    // ERR_INDEX_LOAD_FAILED
    { "Failed to load package index: {}",          "加载包索引失败: {}" },
    // ERR_HOOK_FAILED
    { "Package hook '{}' failed: {}",              "包钩子 '{}' 失败: {}" },
    // ERR_CHECKSUM_MISMATCH
    { "Checksum mismatch for {}",                  "{} 校验和不匹配" },

    // LIST_HEADER
    { "Installed packages:",                       "已安装的包:" },
    // LIST_EMPTY
    { "No packages installed",                     "没有已安装的包" },

    // INFO_HEADER
    { "Package information:",                      "包信息:" },
};
// clang-format on

static_assert(sizeof(gMessages_) / sizeof(gMessages_[0]) == MSG_COUNT,
              "gMessages_ size must match Msg enum count");

export void set_language(const std::string& lang);

export [[nodiscard]] const std::string& language();

export [[nodiscard]] bool is_chinese();

export [[nodiscard]] std::string_view tr(Msg id);

export template<typename... Args>
[[nodiscard]] std::string trf(Msg id, Args&&... args) {
    auto fmt = tr(id);
    return std::vformat(fmt, std::make_format_args(args...));
}

} // namespace xlings::i18n
