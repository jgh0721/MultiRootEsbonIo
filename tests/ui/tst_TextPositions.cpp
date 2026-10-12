#include "TestRunner.hpp"
#include "editor/ScintillaTextPosition.hpp"

#include <QTest>

class TestTextPositions : public QObject
{
    Q_OBJECT
private slots:
    void positionsRoundTrip();
    void completionReplacesPrefix_data();
    void completionReplacesPrefix();
};

void TestTextPositions::positionsRoundTrip()
{
    ScintillaEditBase editor;
    editor.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    editor.send(SCI_SETTABWIDTH, 8);
    const QString document = QString::fromUtf8("\t한글🙂 .. im\n\t:ref:\x60대상\nplain");
    const QByteArray bytes = document.toUtf8();
    editor.sends(SCI_SETTEXT, 0, bytes.constData());
    int line = 0;
    sptr_t first = 0;
    for (const QString& text : document.split(QLatin1Char('\n'))) {
        for (int column = 0; column <= text.size(); ++column) {
            if (column > 0 && column < text.size() && text.at(column - 1).isHighSurrogate()
                && text.at(column).isLowSurrogate())
                continue;
            const sptr_t position = first + text.left(column).toUtf8().size();
            const auto actual = mrst::sci::utf16LineColumn(editor, position);
            QCOMPARE(actual.line, line);
            QCOMPARE(actual.column, column);
            QCOMPARE(mrst::sci::positionFromUtf16LineColumn(editor, line, column), position);
        }
        first += text.toUtf8().size() + 1;
        ++line;
    }
}

void TestTextPositions::completionReplacesPrefix_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("backspace");
    QTest::addColumn<QString>("insert");
    QTest::addColumn<QString>("expected");
    QTest::newRow("tab-directive") << QString("\t.. im") << 2 << QString("image:: ")
        << QString("\t.. image:: ");
    QTest::newRow("emoji-prefix") << QString::fromUtf8("🙂 :ref:\x60ta") << 2 << QString("target")
        << QString::fromUtf8("🙂 :ref:\x60target");
    QTest::newRow("hangul-prefix") << QString::fromUtf8("\t한글 :term:\x60용") << 1 << QString::fromUtf8("용어")
        << QString::fromUtf8("\t한글 :term:\x60용어");
}

void TestTextPositions::completionReplacesPrefix()
{
    QFETCH(QString, text);
    QFETCH(int, backspace);
    QFETCH(QString, insert);
    QFETCH(QString, expected);
    ScintillaEditBase editor;
    editor.send(SCI_SETCODEPAGE, SC_CP_UTF8);
    editor.send(SCI_SETTABWIDTH, 8);
    const QByteArray bytes = text.toUtf8();
    editor.sends(SCI_SETTEXT, 0, bytes.constData());
    const sptr_t caret = bytes.size();
    const auto point = mrst::sci::utf16LineColumn(editor, caret);
    const sptr_t start = mrst::sci::positionFromUtf16LineColumn(editor, point.line, point.column - backspace);
    editor.send(SCI_SETTARGETSTART, start);
    editor.send(SCI_SETTARGETEND, caret);
    const QByteArray replacement = insert.toUtf8();
    editor.sends(SCI_REPLACETARGET, replacement.size(), replacement.constData());
    QByteArray result(static_cast<int>(editor.send(SCI_GETLENGTH)) + 1, '\0');
    editor.send(SCI_GETTEXT, result.size(), reinterpret_cast<sptr_t>(result.data()));
    result.chop(1);
    QCOMPARE(QString::fromUtf8(result), expected);
}

MRST_REGISTER_TEST(TestTextPositions);
#include "tst_TextPositions.moc"
