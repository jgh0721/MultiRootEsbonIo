#include "TestRunner.hpp"
#include "editor/ScintillaSearchWord.hpp"
#include "editor/FindReplaceWidget.hpp"

#include <QTest>

class SearchPrefillTest : public QObject
{
    Q_OBJECT
private slots:
    void wordAtCaret_data();
    void wordAtCaret();
    void typingReplacesPrefilledQuery();
};

void SearchPrefillTest::wordAtCaret_data()
{
    QTest::addColumn<QString>( "text" );
    QTest::addColumn<int>( "caret" );
    QTest::addColumn<QString>( "expected" );
    QTest::newRow( "english" ) << QString( "first second" ) << 8 << QString( "second" );
    QTest::newRow( "korean" ) << QString::fromUtf8( "앞 검색단어 뒤" ) << 4 << QString::fromUtf8( "검색단어" );
    QTest::newRow( "identifier" ) << QString( "some_name42 next" ) << 6 << QString( "some_name42" );
    QTest::newRow( "end" ) << QString( "last" ) << 4 << QString( "last" );
    QTest::newRow( "space" ) << QString( "one   two" ) << 5 << QString{};
    QTest::newRow( "punctuation" ) << QString( "one ... two" ) << 5 << QString{};
    QTest::newRow( "empty" ) << QString{} << 0 << QString{};
}

void SearchPrefillTest::wordAtCaret()
{
    QFETCH( QString, text );
    QFETCH( int, caret );
    QFETCH( QString, expected );
    ScintillaEditBase editor;
    editor.send( SCI_SETCODEPAGE, SC_CP_UTF8 );
    const QByteArray bytes = text.toUtf8();
    editor.send( SCI_SETTEXT, 0, reinterpret_cast<sptr_t>( bytes.constData() ) );
    const sptr_t position = text.left( caret ).toUtf8().size();
    editor.send( SCI_SETSEL, position, position );
    editor.send( SCI_SETSAVEPOINT );
    QCOMPARE( mrst::sci::wordAtCaret( editor ), expected );
    QCOMPARE( editor.send( SCI_GETCURRENTPOS ), position );
    QCOMPARE( editor.send( SCI_GETANCHOR ), position );
    QCOMPARE( editor.send( SCI_GETMODIFY ), 0 );
}

void SearchPrefillTest::typingReplacesPrefilledQuery()
{
    FindReplaceWidget widget;
    widget.show();
    widget.activateWindow();
    widget.setSearchText( QString::fromUtf8( "검색단어" ) );
    widget.focusSearchField();
    QTRY_VERIFY( widget.focusWidget() != nullptr );
    auto* field = qobject_cast<QLineEdit*>( widget.focusWidget() );
    QVERIFY( field != nullptr );
    QCOMPARE( field->selectedText(), QString::fromUtf8( "검색단어" ) );
    QTest::keyClicks( field, "replacement" );
    QCOMPARE( widget.searchText(), QString( "replacement" ) );
    widget.focusSearchField();
    QCOMPARE( field->selectedText(), QString( "replacement" ) );
    QTest::keyClicks( field, "next" );
    QCOMPARE( widget.searchText(), QString( "next" ) );
}

MRST_REGISTER_TEST( SearchPrefillTest );
#include "tst_SearchPrefill.moc"
