#include "stdafx.h"
#include "solSphinxScanner.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

namespace mrst {
namespace fs = std::filesystem;
namespace {

std::set<std::string> defaultExcludedDirs() {
    return {".git", ".hg", ".svn", ".idea", ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox", ".venv", "__pycache__", "_build", "build", ".multiroot"};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string normalizeDocName(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    value = trim(value);
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    static const std::regex suffix(R"(\.(rst|txt|md)$)", std::regex::icase);
    value = std::regex_replace(value, suffix, "");
    return value.empty() ? "index" : value;
}

bool pathIsBelow(const fs::path& path, const fs::path& base) {
    const fs::path candidate = fs::weakly_canonical(path);
    const fs::path root = fs::weakly_canonical(base);
    auto candidateIt = candidate.begin();
    for (auto rootIt = root.begin(); rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *candidateIt != *rootIt) {
            return false;
        }
    }
    return true;
}

int relativeDepth(const fs::path& path, const fs::path& base) {
    if (!pathIsBelow(path, base)) {
        return 1'000'000;
    }
    const fs::path rel = fs::relative(fs::weakly_canonical(path), fs::weakly_canonical(base));
    return static_cast<int>(std::distance(rel.begin(), rel.end()));
}

}  // namespace

QString toQString(const fs::path& path) {
    return QString::fromStdWString(path.wstring());
}

QString toCanonicalQString(const fs::path& path) {
    return QString::fromStdWString(fs::weakly_canonical(path).wstring());
}

fs::path toPath(const QString& text) {
    return fs::path(text.toStdWString());
}

bool SphinxProject::contains(const fs::path& filePath) const {
    return pathIsBelow(filePath, sourcePath) || pathIsBelow(filePath, rootPath);
}

ProjectScanner::ProjectScanner(fs::path workspaceRoot, ScannerSettings settings)
    : workspaceRoot_(fs::weakly_canonical(std::move(workspaceRoot))), settings_(std::move(settings)) {
    excludedDirs_ = defaultExcludedDirs();
    for (const std::string& extra : settings_.excludedDirs) {
        excludedDirs_.insert(lower(extra));
    }
}

std::vector<SphinxProject> ProjectScanner::scan() const {
    std::vector<SphinxProject> projects;
    if (!fs::exists(workspaceRoot_)) {
        return projects;
    }

    std::vector<fs::path> stack{workspaceRoot_};
    while (!stack.empty()) {
        fs::path current = stack.back();
        stack.pop_back();
        std::error_code ec;
        if (fs::is_regular_file(current / "conf.py", ec)) {
            projects.push_back(projectFromConf(current / "conf.py"));
        }
        for (const fs::directory_entry& child : fs::directory_iterator(current, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) {
                break;
            }
            if (child.is_directory(ec) && !isExcludedDirectory(child.path())) {
                stack.push_back(child.path());
            }
        }
    }

    std::sort(projects.begin(), projects.end(), [](const SphinxProject& left, const SphinxProject& right) {
        return lower(left.rootPath.string()) < lower(right.rootPath.string());
    });
    return projects;
}

SphinxProject ProjectScanner::projectFromConf(const fs::path& confPath) const {
    SphinxProject project;
    project.rootPath = fs::weakly_canonical(confPath.parent_path());
    project.confPath = fs::weakly_canonical(confPath);
    project.rootDoc = readRootDoc(confPath);
    project.sourcePath = inferSourcePath(project.rootPath, project.rootDoc);
    project.buildPath = project.rootPath / fs::path(settings_.buildDirName);
    project.projectId = projectIdFor(workspaceRoot_, project.rootPath);
    return project;
}

bool ProjectScanner::isExcludedDirectory(const fs::path& path) const {
    return excludedDirs_.contains(lower(path.filename().string()));
}

std::string readRootDoc(const fs::path& confPath) {
    std::ifstream file(confPath, std::ios::binary);
    if (!file) {
        return "index";
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = stream.str();
    static const std::regex assignment(R"((?:^|\n)\s*(root_doc|master_doc)\s*=\s*['\"]([^'\"]+)['\"])");
    std::string rootDoc = "index";
    for (std::sregex_iterator it(text.begin(), text.end(), assignment), end; it != end; ++it) {
        rootDoc = (*it)[2].str();
    }
    return normalizeDocName(rootDoc);
}

fs::path inferSourcePath(const fs::path& rootPath, const std::string& rootDoc) {
    const fs::path rootDocPath(rootDoc);
    if (!rootDocPath.has_parent_path()) {
        return fs::weakly_canonical(rootPath);
    }
    const fs::path possible = rootPath / rootDocPath.parent_path();
    std::error_code ec;
    return fs::exists(possible, ec) ? fs::weakly_canonical(possible) : fs::weakly_canonical(rootPath);
}

std::wstring projectIdFor(const fs::path& workspaceRoot, const fs::path& rootPath) {
    std::error_code ec;
    fs::path rel = fs::relative(fs::weakly_canonical(rootPath), fs::weakly_canonical(workspaceRoot), ec);
    if (ec || rel.empty() || rel == ".") {
        return fs::weakly_canonical(workspaceRoot).filename().wstring();
    }
    std::wstring id;
    for (const fs::path& part : rel) {
        if (!id.empty()) {
            id += L'.';
        }
        id += part.wstring();
    }
    return id.empty() ? fs::weakly_canonical(workspaceRoot).filename().wstring() : id;
}

const SphinxProject* resolveProjectForFile(const fs::path& filePath, const std::vector<SphinxProject>& projects) {
    const SphinxProject* best = nullptr;
    std::pair<int, int> bestScore{1'000'000, 1'000'000};
    for (const SphinxProject& project : projects) {
        if (!project.contains(filePath)) {
            continue;
        }
        const std::pair<int, int> score{relativeDepth(filePath, project.sourcePath), relativeDepth(filePath, project.rootPath)};
        if (score < bestScore) {
            bestScore = score;
            best = &project;
        }
    }
    return best;
}

}  // namespace mrst

