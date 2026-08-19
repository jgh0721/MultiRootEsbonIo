#include "stdafx.h"
#include "TextLexerRegistry.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QDebug>

#include <utility>

namespace {

constexpr auto kLexerRegistryResourcePath = ":/text/lexers.json";

QString lexerRegistryResourcePath()
{
    return QString::fromLatin1(kLexerRegistryResourcePath);
}

QString normalizedLanguageName(const QString& language)
{
    return language.trimmed().isEmpty() ? QStringLiteral("None") : language.trimmed();
}

} // namespace

const TextLexerRegistry& TextLexerRegistry::instance()
{
    static const TextLexerRegistry registry = load();
    return registry;
}

QString TextLexerRegistry::languageForExtension(const QString& extension) const
{
    const QString normalized = extension.trimmed().toLower();
    return m_languageByExtension.value(normalized, QStringLiteral("None"));
}

QString TextLexerRegistry::lexerKeyForDisplayName(const QString& displayName) const
{
    const TextLexerProfile* profile = profileForDisplayName(displayName);
    return profile ? profile->lexerKey : QStringLiteral("none");
}

QStringList TextLexerRegistry::displayNames() const
{
    QStringList names;
    names.reserve(m_profiles.size());
    for (const TextLexerProfile& profile : m_profiles)
        names.append(profile.displayName);
    return names;
}

QStringList TextLexerRegistry::supportedExtensions() const
{
    QSet<QString> deduped;
    for (const TextLexerProfile& profile : m_profiles) {
        for (const QString& extension : profile.extensions)
            deduped.insert(extension.toLower());
    }

    QStringList extensions(deduped.begin(), deduped.end());
    extensions.sort();
    return extensions;
}

const TextLexerProfile* TextLexerRegistry::profileForDisplayName(const QString& displayName) const
{
    const auto it = m_profileIndexByName.constFind(normalizedLanguageName(displayName));
    if (it == m_profileIndexByName.cend())
        return nullptr;
    return &m_profiles[it.value()];
}

TextLexerRegistry TextLexerRegistry::load()
{
    TextLexerRegistry registry;

    QFile file(lexerRegistryResourcePath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        const QJsonArray languages = document.object().value(QStringLiteral("languages")).toArray();
        QVector<TextLexerProfile> loadedProfiles;
        loadedProfiles.reserve(languages.size());

        for (const QJsonValue& value : languages) {
            const QJsonObject object = value.toObject();
            const QString displayName = normalizedLanguageName(object.value(QStringLiteral("displayName")).toString());
            const QString lexerKey = object.value(QStringLiteral("lexerKey")).toString();
            if (displayName.isEmpty() || lexerKey.isEmpty())
                continue;

            QStringList extensions;
            const QJsonArray extensionArray = object.value(QStringLiteral("extensions")).toArray();
            for (const QJsonValue& extensionValue : extensionArray) {
                const QString extension = extensionValue.toString().trimmed().toLower();
                if (!extension.isEmpty())
                    extensions.append(extension);
            }

            loadedProfiles.append(TextLexerProfile{
                object.value(QStringLiteral("languageId")).toString(displayName.toLower()),
                displayName,
                lexerKey,
                extensions
            });
        }

        if (!loadedProfiles.isEmpty()) {
            registry.m_profiles = std::move(loadedProfiles);
            registry.m_loadedFromJson = true;
            registry.m_sourceDescription = lexerRegistryResourcePath();
            registry.buildIndexes();
            return registry;
        }

        qWarning().noquote() << QStringLiteral("[TextLexerRegistry] Resource '%1' did not contain usable language entries. Falling back to built-in table.")
                                .arg(lexerRegistryResourcePath());
    } else {
        qWarning().noquote() << QStringLiteral("[TextLexerRegistry] Failed to open '%1'. Falling back to built-in table.")
                                .arg(lexerRegistryResourcePath());
    }

    registry.m_profiles = fallbackProfiles();
    registry.m_loadedFromJson = false;
    registry.m_sourceDescription = QStringLiteral("built-in fallback");
    registry.buildIndexes();
    return registry;
}

QVector<TextLexerProfile> TextLexerRegistry::fallbackProfiles()
{
    return {
        {QStringLiteral("none"), QStringLiteral("None"), QStringLiteral("none"),
            {QStringLiteral("txt"), QStringLiteral("log"), QStringLiteral("ini"), QStringLiteral("cfg"),
             QStringLiteral("php"), QStringLiteral("rb"), QStringLiteral("go"),
             QStringLiteral("rs")}},
        // Lexilla 에 reStructuredText 렉서가 없어 자체 컨테이너 렉서를 쓴다.
        // lexerKey "rst-container" 는 ScintillaQtDirectBackend 가 특별 취급한다.
        {QStringLiteral("restructuredtext"), QStringLiteral("reStructuredText"), QStringLiteral("rst-container"),
            {QStringLiteral("rst"), QStringLiteral("rest")}},
        {QStringLiteral("toml"), QStringLiteral("TOML"), QStringLiteral("toml"), {QStringLiteral("toml")}},
        {QStringLiteral("cpp"), QStringLiteral("C++"), QStringLiteral("cpp"),
            {QStringLiteral("cpp"), QStringLiteral("c"), QStringLiteral("h"), QStringLiteral("hpp"), QStringLiteral("cxx")}},
        {QStringLiteral("python"), QStringLiteral("Python"), QStringLiteral("python"), {QStringLiteral("py")}},
        {QStringLiteral("javascript"), QStringLiteral("JavaScript"), QStringLiteral("cpp"), {QStringLiteral("js"), QStringLiteral("ts")}},
        {QStringLiteral("html"), QStringLiteral("HTML"), QStringLiteral("hypertext"), {QStringLiteral("html"), QStringLiteral("htm")}},
        {QStringLiteral("css"), QStringLiteral("CSS"), QStringLiteral("css"), {QStringLiteral("css")}},
        {QStringLiteral("java"), QStringLiteral("Java"), QStringLiteral("cpp"), {QStringLiteral("java"), QStringLiteral("cs")}},
        {QStringLiteral("xml"), QStringLiteral("XML"), QStringLiteral("xml"), {QStringLiteral("xml")}},
        {QStringLiteral("json"), QStringLiteral("JSON"), QStringLiteral("json"), {QStringLiteral("json")}},
        {QStringLiteral("bash"), QStringLiteral("Bash"), QStringLiteral("bash"), {QStringLiteral("sh")}},
        {QStringLiteral("batch"), QStringLiteral("Batch"), QStringLiteral("batch"), {QStringLiteral("bat")}},
        {QStringLiteral("yaml"), QStringLiteral("YAML"), QStringLiteral("yaml"), {QStringLiteral("yaml"), QStringLiteral("yml")}},
        {QStringLiteral("sql"), QStringLiteral("SQL"), QStringLiteral("sql"), {QStringLiteral("sql")}},
        {QStringLiteral("markdown"), QStringLiteral("Markdown"), QStringLiteral("markdown"),
            {QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("mdown")}}
    };
}

void TextLexerRegistry::buildIndexes()
{
    m_profileIndexByName.clear();
    m_languageByExtension.clear();

    // ⚠ displayName 은 **번역 금지**다. 화면(도구모음 언어 콤보)에 보이기도
    //   하지만 동시에 m_profileIndexByName 의 키이고, 콤보의 currentTextChanged
    //   가 그 텍스트를 그대로 setLanguage() 로 되돌려 준다. 번역하는 순간 언어
    //   선택이 아무 프로파일도 찾지 못한다. 값들은 어차피 고유명사다
    //   (C++, Python, Markdown, None...).
    for (qsizetype i = 0; i < m_profiles.size(); ++i) {
        const TextLexerProfile& profile = m_profiles[i];
        const QString displayName = normalizedLanguageName(profile.displayName);
        if (!m_profileIndexByName.contains(displayName))
            m_profileIndexByName.insert(displayName, static_cast<int>(i));

        for (const QString& extension : profile.extensions) {
            const QString normalized = extension.trimmed().toLower();
            if (!normalized.isEmpty() && !m_languageByExtension.contains(normalized))
                m_languageByExtension.insert(normalized, displayName);
        }
    }

    if (!m_profileIndexByName.contains(QStringLiteral("None"))) {
        const int index = m_profiles.size();
        m_profiles.append(TextLexerProfile{QStringLiteral("none"), QStringLiteral("None"), QStringLiteral("none"), {}});
        m_profileIndexByName.insert(QStringLiteral("None"), index);
    }
}


