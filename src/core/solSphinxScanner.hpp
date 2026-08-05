#pragma once

#include <QString>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace mrst {

/// std::filesystem::path <-> QString 변환. Qt 경계마다 파일 로컬 static 으로
/// 중복 정의되던 것을 한 곳으로 모았다.
[[nodiscard]] QString toQString(const std::filesystem::path& path);
[[nodiscard]] QString toCanonicalQString(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path toPath(const QString& text);

struct SphinxProject {
    std::wstring projectId;
    std::filesystem::path rootPath;
    std::filesystem::path confPath;
    std::filesystem::path sourcePath;
    std::string rootDoc = "index";
    std::filesystem::path buildPath;

    [[nodiscard]] bool contains(const std::filesystem::path& filePath) const;
};

struct ScannerSettings {
    std::string buildDirName = "_build/multiroot-rest";
    std::vector<std::string> excludedDirs;
};

class ProjectScanner final {
public:
    explicit ProjectScanner(std::filesystem::path workspaceRoot, ScannerSettings settings = {});

    [[nodiscard]] std::vector<SphinxProject> scan() const;
    [[nodiscard]] SphinxProject projectFromConf(const std::filesystem::path& confPath) const;

private:
    std::filesystem::path workspaceRoot_;
    ScannerSettings settings_;
    std::set<std::string> excludedDirs_;   // 생성자에서 1회 구성 (소문자 정규화됨)

    [[nodiscard]] bool isExcludedDirectory(const std::filesystem::path& path) const;
};

[[nodiscard]] std::string readRootDoc(const std::filesystem::path& confPath);

/// conf.py 에 `html_style = ''` 이 있는지. Sphinx 8 에서 _static checksum
/// 오류를 내므로 configOverrides 로 None 을 강제해야 한다.
///
/// 정규식이라 한계가 있다: 변수 대입(html_style = STYLE), 조건부 대입,
/// 삼중따옴표 문자열 안의 텍스트는 잡지 못한다. 정확한 판정은 Python
/// 빌더의 --auto-fix-legacy-conf(ast 파싱)가 하고, 이 함수는 런타임이 아직
/// 준비되지 않은 시점의 폴백이다.
[[nodiscard]] bool confDeclaresEmptyHtmlStyle(const std::filesystem::path& confPath);
[[nodiscard]] std::filesystem::path inferSourcePath(const std::filesystem::path& rootPath, const std::string& rootDoc);
[[nodiscard]] std::wstring projectIdFor(const std::filesystem::path& workspaceRoot, const std::filesystem::path& rootPath);
[[nodiscard]] const SphinxProject* resolveProjectForFile(const std::filesystem::path& filePath, const std::vector<SphinxProject>& projects);

}  // namespace mrst

