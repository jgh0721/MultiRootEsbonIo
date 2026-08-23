#include "stdafx.h"
#include "uis/dlgSphinxBuild.hpp"

#include "core/solAppSettings.hpp"
#include "core/solSphinxBuilders.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace {

constexpr auto kLastBuilderKey = "build/lastBuilder";
constexpr auto kOpenWhenDoneKey = "build/openWhenDone";

}  // namespace

QSphinxBuildDialog::QSphinxBuildDialog( const QString& projectLabel, const QString& projectRoot,
                                        QWidget* parent )
    : QDialog( parent )
    , projectLabel_( projectLabel )
    , projectRoot_( projectRoot )
{
    buildUi();
    retranslateUi();

    const AppSettings settings;
    const QString lastBuilder = settings.value( QLatin1String( kLastBuilderKey ),
                                               QStringLiteral( "html" ) ).toString();
    builderCombo_->setCurrentText( mrst::isValidSphinxBuilderName( lastBuilder )
                                       ? lastBuilder
                                       : QStringLiteral( "html" ) );
    openCheck_->setChecked( settings.value( QLatin1String( kOpenWhenDoneKey ), true ).toBool() );

    // 경로는 기억하지 않는다. 빌더마다 다른 것이 기본이고, 프로젝트가 바뀌면
    // 지난 프로젝트의 경로가 남아 엉뚱한 곳에 쓰게 된다.
    outputEdit_->setText( mrst::defaultSphinxOutputDirectory( projectRoot_,
                                                             builderCombo_->currentText() ) );
    outputEdited_ = false;
    updateOkState();
}

void QSphinxBuildDialog::buildUi()
{
    setModal( true );
    setSizeGripEnabled( true );

    builderLabel_ = new QLabel( this );
    builderCombo_ = new QComboBox( this );
    builderCombo_->setEditable( true );
    builderCombo_->addItems( mrst::sphinxBuilderPresets() );
    builderCombo_->setInsertPolicy( QComboBox::NoInsert );
    builderLabel_->setBuddy( builderCombo_ );

    outputLabel_ = new QLabel( this );
    outputEdit_ = new QLineEdit( this );
    outputEdit_->setMinimumWidth( 360 );
    outputLabel_->setBuddy( outputEdit_ );
    browseButton_ = new QPushButton( this );
    browseButton_->setAutoDefault( false );

    auto* outputRow = new QHBoxLayout;
    outputRow->setContentsMargins( 0, 0, 0, 0 );
    outputRow->addWidget( outputEdit_, 1 );
    outputRow->addWidget( browseButton_ );

    auto* form = new QFormLayout;
    form->addRow( builderLabel_, builderCombo_ );
    form->addRow( outputLabel_, outputRow );

    openCheck_ = new QCheckBox( this );
    hintLabel_ = new QLabel( this );
    hintLabel_->setWordWrap( true );
    QFont hintFont = hintLabel_->font();
    hintFont.setPointSizeF( hintFont.pointSizeF() * 0.92 );
    hintLabel_->setFont( hintFont );
    hintLabel_->setEnabled( false );   // 팔레트의 Disabled 색 = 보조 문구

    buttons_ = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    buildButton_ = buttons_->button( QDialogButtonBox::Ok );
    buildButton_->setDefault( true );

    auto* root = new QVBoxLayout( this );
    root->addLayout( form );
    root->addWidget( openCheck_ );
    root->addWidget( hintLabel_ );
    root->addStretch( 1 );
    root->addWidget( buttons_ );

    connect( builderCombo_, &QComboBox::currentTextChanged, this,
            &QSphinxBuildDialog::onBuilderChanged );
    connect( outputEdit_, &QLineEdit::textEdited, this, [ this ] {
        outputEdited_ = true;
        updateOkState();
    } );
    connect( browseButton_, &QPushButton::clicked, this, &QSphinxBuildDialog::onBrowseOutput );
    connect( buttons_, &QDialogButtonBox::accepted, this, [ this ] {
        rememberChoices();
        accept();
    } );
    connect( buttons_, &QDialogButtonBox::rejected, this, &QSphinxBuildDialog::reject );
}

void QSphinxBuildDialog::retranslateUi()
{
    setWindowTitle( projectLabel_.isEmpty() ? tr( "빌드" )
                                            : tr( "빌드 — %1" ).arg( projectLabel_ ) );
    builderLabel_->setText( tr( "빌더(&B):" ) );
    outputLabel_->setText( tr( "출력 위치(&O):" ) );
    browseButton_->setText( tr( "찾아보기…" ) );
    openCheck_->setText( tr( "빌드가 끝나면 그 폴더를 탐색기로 열기" ) );
    hintLabel_->setText(
        tr( "목록에 없는 빌더는 직접 입력할 수 있습니다. 출력 위치가 없으면 만들어집니다.\n"
            "latexpdf · info 는 빌더가 아니라 make 목표라 산출물이 출력 위치 아래의 "
            "하위 폴더에 놓입니다. PDF 는 LaTeX 도구체인이 설치돼 있어야 합니다." ) );
    if( buildButton_ != nullptr )
        buildButton_->setText( tr( "빌드" ) );
    if( QPushButton* cancel = buttons_->button( QDialogButtonBox::Cancel ); cancel != nullptr )
        cancel->setText( tr( "취소" ) );
}

void QSphinxBuildDialog::changeEvent( QEvent* event )
{
    if( event != nullptr && event->type() == QEvent::LanguageChange )
        retranslateUi();
    QDialog::changeEvent( event );
}

QString QSphinxBuildDialog::builder() const
{
    return builderCombo_->currentText().trimmed();
}

QString QSphinxBuildDialog::outputDirectory() const
{
    return outputEdit_->text().trimmed();
}

bool QSphinxBuildDialog::openWhenDone() const
{
    return openCheck_->isChecked();
}

void QSphinxBuildDialog::rememberChoices() const
{
    AppSettings settings;
    settings.setValue( QLatin1String( kLastBuilderKey ), builder() );
    settings.setValue( QLatin1String( kOpenWhenDoneKey ), openWhenDone() );
}

void QSphinxBuildDialog::onBuilderChanged()
{
    if( !outputEdited_ )
        outputEdit_->setText( mrst::defaultSphinxOutputDirectory( projectRoot_, builder() ) );
    updateOkState();
}

void QSphinxBuildDialog::onBrowseOutput()
{
    const QString start = outputDirectory().isEmpty() ? projectRoot_ : outputDirectory();
    const QString chosen = QFileDialog::getExistingDirectory(
        this, tr( "빌드 결과를 놓을 폴더" ), start,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks );
    if( chosen.isEmpty() )
        return;   // 사용자가 취소했다

    outputEdit_->setText( QDir::toNativeSeparators( chosen ) );
    outputEdited_ = true;
    updateOkState();
}

void QSphinxBuildDialog::updateOkState()
{
    if( buildButton_ == nullptr )
        return;

    buildButton_->setEnabled( mrst::isValidSphinxBuilderName( builder() )
                             && !outputDirectory().isEmpty() );
}
