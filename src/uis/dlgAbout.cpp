#include "stdafx.h"
#include "dlgAbout.hpp"

#include "core/solThemeManager.hpp"
#include "core/solUpdateManifest.hpp"
#include "core/solUpdateService.hpp"
#include "utils/DwmTitleBar.hpp"

namespace
{
    /// 아이콘 원본은 512px 이라 어느 배율에서도 축소만 일어난다.
    constexpr int kIconSide = 96;

    /// 값 쪽 라벨. 버전 문자열을 복사해 갈 수 있어야 오류 보고에 쓸 수 있다
    /// (설정 대화상자의 값 라벨과 같은 규칙이다).
    QLabel* createValueLabel( QWidget* parent )
    {
        auto* label = new QLabel( parent );
        label->setTextInteractionFlags( Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard );
        return label;
    }
}  // namespace

QAboutDialog::QAboutDialog( mrst::UpdateService* Service, QWidget* Parent )
    : QDialog( Parent )
    , updateService_( Service )
{
    setObjectName( QStringLiteral( "dlgAbout" ) );
    // 내용이 고정된 창이라 늘릴 이유가 없다. 손잡이만 없애고 크기 자체는
    // 잠그지 않는다 — 최신 버전 줄의 길이가 상태마다 달라진다.
    setSizeGripEnabled( false );

    buildUi();
    retranslateUi();
    connectUpdateService();

    // 설정에서 테마를 바꾸면(미리보기 포함) 제목 표시줄도 따라가야 한다.
    connect( &ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this]( ThemeManager::Theme ) { applyTitleBarTheme(); } );

    // 네이티브 창을 미리 만들어 두면 첫 표시 전에 DWM 속성을 걸 수 있다
    // (창이 뜬 뒤에 걸면 밝은 제목 표시줄이 한 번 번쩍인다).
    createWinId();
    applyTitleBarTheme();
}

void QAboutDialog::buildUi()
{
    auto* root = new QVBoxLayout( this );
    root->setContentsMargins( 24, 20, 24, 16 );
    root->setSpacing( 10 );

    iconLabel_ = new QLabel( this );
    iconLabel_->setAlignment( Qt::AlignCenter );
    // 배율 계산은 QIcon 에 맡긴다. QPixmap::scaled() 로 직접 줄이면 고DPI 에서
    // devicePixelRatio 가 붙지 않아 흐릿하게 그려진다.
    iconLabel_->setPixmap( QIcon( QStringLiteral( ":/icons/app.png" ) )
                           .pixmap( QSize( kIconSide, kIconSide ), devicePixelRatioF() ) );
    root->addWidget( iconLabel_ );

    // 제품 이름은 옮기지 않는다(창 제목과 같은 규칙). tr() 로 감싸면 번역할
    // 문자열이 하나 늘어날 뿐이라 main() 이 설정한 값을 그대로 쓴다.
    productLabel_ = new QLabel( QCoreApplication::applicationName(), this );
    productLabel_->setAlignment( Qt::AlignCenter );
    productLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );
    QFont productFont = productLabel_->font();
    productFont.setBold( true );
    productFont.setPointSizeF( productFont.pointSizeF() * 1.2 );
    productLabel_->setFont( productFont );
    root->addWidget( productLabel_ );

    root->addSpacing( 6 );

    auto* form = new QFormLayout;
    form->setHorizontalSpacing( 12 );
    form->setVerticalSpacing( 8 );
    // 값이 길어져도 라벨과 값이 위아래로 갈라지지 않게 한다.
    form->setRowWrapPolicy( QFormLayout::DontWrapRows );
    form->setLabelAlignment( Qt::AlignRight | Qt::AlignVCenter );

    currentCaption_ = new QLabel( this );
    currentValue_   = createValueLabel( this );
    form->addRow( currentCaption_, currentValue_ );

    latestCaption_ = new QLabel( this );

    auto* latestRow    = new QWidget( this );
    auto* latestLayout = new QHBoxLayout( latestRow );
    latestLayout->setContentsMargins( 0, 0, 0, 0 );
    latestLayout->setSpacing( 8 );
    latestValue_ = createValueLabel( latestRow );
    latestLayout->addWidget( latestValue_, 1 );
    checkButton_ = new QPushButton( latestRow );
    latestLayout->addWidget( checkButton_ );
    form->addRow( latestCaption_, latestRow );

    repositoryCaption_ = new QLabel( this );
    repositoryValue_   = createValueLabel( this );
    repositoryValue_->setTextInteractionFlags( Qt::TextBrowserInteraction );
    const QString repository = mrst::repositoryUrl().toString();
    repositoryValue_->setText( QStringLiteral( "<a href=\"%1\">%1</a>" ).arg( repository ) );
    // QLabel 이 직접 열게 두지 않는다(setOpenExternalLinks). 여는 주체가 우리
    // 코드여야 업데이트 알림 바의 릴리스 노트와 같은 경로를 쓴다.
    connect( repositoryValue_, &QLabel::linkActivated, this,
            []( const QString& Link ) { QDesktopServices::openUrl( QUrl( Link ) ); } );
    form->addRow( repositoryCaption_, repositoryValue_ );

    root->addLayout( form );
    root->addStretch( 1 );

    buttonBox_ = new QDialogButtonBox( QDialogButtonBox::Ok, this );
    connect( buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept );
    root->addWidget( buttonBox_ );

    // QPushButton 은 QDialog 안에서 autoDefault 가 켜진 채로 만들어지고, 기본
    // 버튼은 **먼저 만들어진 쪽**이 가져간다. 그대로 두면 [지금 확인] 이 기본이
    // 되어 Enter 가 창을 닫는 대신 네트워크 점검을 시작한다(Qlementine 이
    // 강조색까지 그쪽에 칠해 눈으로도 주 동작처럼 보인다).
    checkButton_->setAutoDefault( false );
    if( auto* ok = buttonBox_->button( QDialogButtonBox::Ok ) )
    {
        ok->setDefault( true );
        ok->setFocus();
    }

    connect( checkButton_, &QPushButton::clicked, this, [this] {
        if( updateService_.isNull() )
            return;
        emit updateCheckRequested();
        // 상태를 여기서 미리 "확인 중" 으로 바꾸지 않는다. checkAsync() 는 이미
        // 진행 중이면 시그널 하나 없이 조용히 되돌아가므로, 그 경우 화면이
        // 영영 "확인 중" 에 멈춘다. 실제 전환은 stateChanged 가 알려 준다.
        refreshUpdateStatus();
    } );

    setMinimumWidth( 380 );
}

void QAboutDialog::connectUpdateService()
{
    if( updateService_.isNull() )
        return;

    connect( updateService_, &mrst::UpdateService::stateChanged, this,
            [this]( const mrst::UpdateService::State State ) {
                // 새 점검이 시작됐으면 지난 결과를 지운다.
                if( State == mrst::UpdateService::State::Checking )
                {
                    checkFailed_ = false;
                    noRelease_   = false;
                    failureText_.clear();
                }
                refreshUpdateStatus();
            } );

    // silent 인 실패(배경 점검)도 받는다. 여기는 알림이 아니라 상태 표시라,
    // 확인이 실패한 사실 자체는 보여 주는 편이 맞다.
    connect( updateService_, &mrst::UpdateService::failed, this,
            [this]( const QString& Message, bool ) {
                checkFailed_ = true;
                failureText_ = Message;
                refreshUpdateStatus();
            } );

    // 정식 릴리스가 하나도 없으면(매니페스트 404) available() 이 빈 채로
    // upToDate 가 온다. "확인하지 않음" 과 구분하려면 그 순간을 잡아야 한다.
    connect( updateService_, &mrst::UpdateService::upToDate, this, [this]( bool ) {
        noRelease_ = !updateService_->available().isValid();
        refreshUpdateStatus();
    } );
}

void QAboutDialog::retranslateUi()
{
    //: 정보 대화상자의 제목. %1 은 제품 이름이다.
    setWindowTitle( tr( "%1 정보" ).arg( QCoreApplication::applicationName() ) );

    currentCaption_->setText( tr( "현재 버전:" ) );
    latestCaption_->setText( tr( "최신 버전:" ) );
    repositoryCaption_->setText( tr( "저장소:" ) );

    currentValue_->setText( QCoreApplication::applicationVersion() );
    checkButton_->setText( tr( "지금 확인" ) );

    if( auto* ok = buttonBox_->button( QDialogButtonBox::Ok ) )
        ok->setText( tr( "확인(&O)" ) );

    refreshUpdateStatus();
}

void QAboutDialog::refreshUpdateStatus()
{
    if( latestValue_ == nullptr )
        return;

    const bool             haveService = !updateService_.isNull();
    const mrst::UpdateInfo info        = haveService ? updateService_->available() : mrst::UpdateInfo{};
    const Latest           latest      = latestState();

    QString text;
    switch( latest )
    {
        case Latest::Checking:   text = tr( "확인 중..." ); break;
        case Latest::UpToDate:   text = tr( "%1 (최신 버전입니다)" ).arg( info.version ); break;
        case Latest::NewVersion: text = tr( "%1 (새 버전이 있습니다)" ).arg( info.version ); break;
        case Latest::NoRelease:  text = tr( "배포된 릴리스가 없습니다" ); break;
        case Latest::Failed:     text = tr( "확인하지 못했습니다" ); break;
        case Latest::Unknown:    text = tr( "확인하지 않음" ); break;
    }
    latestValue_->setText( text );

    // 실패 사유와 마지막 확인 시각은 창을 넓히지 않도록 툴팁으로 둔다.
    QStringList hints;
    if( latest == Latest::Failed && !failureText_.isEmpty() )
        hints << failureText_;
    if( haveService )
    {
        // 저장은 UTC ISO 문자열이다. 사람에게는 지역 시각으로 보여 준다
        // (설정 대화상자의 "마지막 확인" 과 같은 형식).
        const QDateTime last = updateService_->lastCheckedAt();
        hints << tr( "마지막 확인: %1" )
                    .arg( last.isValid()
                             ? last.toLocalTime().toString( QStringLiteral( "yyyy-MM-dd HH:mm" ) )
                             : tr( "(없음)" ) );
    }
    latestValue_->setToolTip( hints.join( QLatin1Char( '\n' ) ) );

    // 진행 중에는 다시 눌러도 checkAsync() 가 조용히 되돌아간다. 설치 준비가
    // 끝난 뒤도 마찬가지다 — 그때는 더 확인할 것이 없다.
    checkButton_->setEnabled( haveService && !updateService_->isBusy()
                              && updateService_->state() != mrst::UpdateService::State::ReadyToInstall );
}

QAboutDialog::Latest QAboutDialog::latestState() const
{
    if( updateService_.isNull() )
        return Latest::Unknown;

    if( updateService_->state() == mrst::UpdateService::State::Checking )
        return Latest::Checking;

    // 마지막 점검이 실패했으면 그것이 지금의 사실이다. 그 전에 받아 둔
    // available() 을 보여 주면 방금 실패한 확인이 성공한 것처럼 읽힌다.
    if( checkFailed_ )
        return Latest::Failed;

    const mrst::UpdateInfo info = updateService_->available();
    if( info.isValid() )
    {
        // 최신이어도 info_ 는 채워진다(UpdateService::onManifestFinished 가
        // 버전 비교보다 **먼저** 대입한다). 그래서 여기서 다시 비교해야 한다.
        return info.isNewerThan( QCoreApplication::applicationVersion() ) ? Latest::NewVersion
                                                                         : Latest::UpToDate;
    }

    if( noRelease_ )
        return Latest::NoRelease;

    return Latest::Unknown;
}

void QAboutDialog::requestCheckIfIdle()
{
    if( updateService_.isNull() )
        return;
    // 자동 확인을 꺼 둔 사용자에게는 창을 여는 것만으로 네트워크를 타지 않는다.
    // 그래도 [지금 확인] 은 살아 있으므로 원하면 직접 누를 수 있다.
    if( updateService_->checkIntervalDays() <= 0 )
        return;
    if( updateService_->isBusy() )
        return;
    if( updateService_->state() == mrst::UpdateService::State::ReadyToInstall )
        return;
    // 이번 실행에서 이미 확인했다. available() 은 최신일 때도 채워진다.
    if( updateService_->available().isValid() )
        return;

    emit updateCheckRequested();
}

void QAboutDialog::applyTitleBarTheme()
{
    auto& themeManager = ThemeManager::instance();
    DwmTitleBar::applyTheme( this, themeManager.currentTheme() == ThemeManager::Dark,
                            themeManager.toolBarColor() );
}

void QAboutDialog::showEvent( QShowEvent* Event )
{
    QDialog::showEvent( Event );

    // 설정 대화상자와 같은 보험이다. 지금은 onAbout() 이 열 때마다 새로 만들어
    // 생성자의 적용만으로 충분하지만(라이트에서 제목 표시줄이 #f9f9f9 로 찍히는
    // 것을 확인했다), 인스턴스를 캐시하도록 바뀌면 닫혀 있는 동안 바뀐 테마를
    // 여기서만 따라잡을 수 있다.
    applyTitleBarTheme();

    if( !firstShow_ )
        return;
    firstShow_ = false;

    requestCheckIfIdle();
    refreshUpdateStatus();
}

void QAboutDialog::changeEvent( QEvent* Event )
{
    if( Event != nullptr && Event->type() == QEvent::LanguageChange )
        retranslateUi();

    QDialog::changeEvent( Event );
}
