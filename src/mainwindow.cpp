#include "mainwindow.h"
#include <QSplitter>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QListWidgetItem>
#include <QSettings>
#include <QTimer>
#include <QKeyEvent>
#include <QDesktopServices>
#include <QActionGroup>
#include <QSystemTrayIcon>
#include <QShortcut>
#include <QPrinter>
#include <QPrintDialog>
#include <QMimeData>
#include <QTextBlock>
#include <QClipboard>
#include <QTemporaryFile>
#include <QScrollBar>
#include <QTextDocumentFragment>
#include <QProcess>
#include "ui_mainwindow.h"
#include "dialogs/linkdialog.h"
#include "services/owncloudservice.h"
#include "services/databaseservice.h"
#include "dialogs/tododialog.h"
#include "libraries/diff_match_patch/diff_match_patch.h"
#include "dialogs/notediffdialog.h"
#include "build_number.h"
#include "version.h"
#include "release.h"
#include "dialogs/aboutdialog.h"
#include "dialogs/settingsdialog.h"
#include "entities/calendaritem.h"
#include "widgets/qownnotesmarkdowntextedit.h"
#include "dialogs/passworddialog.h"
#include "services/metricsservice.h"
#include <services/cryptoservice.h>
#include <helpers/clientproxy.h>
#include <utils/misc.h>
#include <entities/notefolder.h>
#include <entities/tag.h>


MainWindow::MainWindow(QWidget *parent) :
        QMainWindow(parent),
        ui(new Ui::MainWindow) {
    ui->setupUi(this);
    this->setWindowTitle(
            "QOwnNotes - version " + QString(VERSION) +
                    " - build " + QString::number(BUILD));

    ClientProxy proxy;
    // refresh the Qt proxy settings
    proxy.setupQtProxyFromSettings();

    QActionGroup *sorting = new QActionGroup(this);
    sorting->addAction(ui->actionAlphabetical);
    sorting->addAction(ui->actionBy_date);
    sorting->setExclusive(true);

    // hide the encrypted note text edit by default
    ui->encryptedNoteTextEdit->hide();

    // set the search frames for the note text edits
    ui->noteTextEdit->initSearchFrame(ui->noteTextEditSearchFrame);
    ui->encryptedNoteTextEdit->initSearchFrame(ui->noteTextEditSearchFrame);

    // set the main window for accessing it's public methods
    ui->noteTextEdit->setMainWindow(this);
    ui->encryptedNoteTextEdit->setMainWindow(this);

    DatabaseService::createConnection();
    DatabaseService::setupTables();

    this->firstVisibleNoteListRow = 0;
    this->noteHistory = NoteHistory();

    // set our signal mapper
    this->recentNoteFolderSignalMapper = new QSignalMapper(this);

    // initialize the toolbars
    initToolbars();

    readSettings();

    // set sorting
    ui->actionBy_date->setChecked(!sortAlphabetically);
    ui->actionAlphabetical->setChecked(sortAlphabetically);

    // set the show in system tray checkbox
    ui->actionShow_system_tray->setChecked(showSystemTray);

    createSystemTrayIcon();
    initMainSplitter();
    buildNotesIndex();
    loadNoteDirectoryList();

    // setup the update available button
    setupUpdateAvailableButton();

    this->noteDiffDialog = new NoteDiffDialog();

    // look if we need to save something every 10 sec (default)
    this->noteSaveTimer = new QTimer(this);
    QObject::connect(
            this->noteSaveTimer,
            SIGNAL(timeout()),
            this,
            SLOT(storeUpdatedNotesToDisk()));
    this->noteSaveTimer->start(this->noteSaveIntervalTime * 1000);

    // look if we need update the note view every two seconds
    _noteViewUpdateTimer = new QTimer(this);
    QObject::connect(
            _noteViewUpdateTimer,
            SIGNAL(timeout()),
            this,
            SLOT(noteViewUpdateTimerSlot()));
    _noteViewUpdateTimer->start(2000);

    // check if we have a todo reminder every minute
    this->todoReminderTimer = new QTimer(this);
    QObject::connect(
            this->todoReminderTimer,
            SIGNAL(timeout()),
            this,
            SLOT(frequentPeriodicChecker()));
    this->todoReminderTimer->start(60000);

    QObject::connect(
            &this->noteDirectoryWatcher,
            SIGNAL(directoryChanged(QString)),
            this,
            SLOT(notesDirectoryWasModified(QString)));
    QObject::connect(
            &this->noteDirectoryWatcher,
            SIGNAL(fileChanged(QString)),
            this,
            SLOT(notesWereModified(QString)));
    ui->searchLineEdit->installEventFilter(this);
    ui->notesListWidget->installEventFilter(this);
    ui->noteTextEdit->installEventFilter(this);
    ui->noteTextEdit->viewport()->installEventFilter(this);
    ui->encryptedNoteTextEdit->installEventFilter(this);
    ui->encryptedNoteTextEdit->viewport()->installEventFilter(this);
    ui->tagListWidget->installEventFilter(this);
    ui->notesListWidget->setCurrentRow(0);

    // ignores note clicks in QMarkdownTextEdit in the note text edit
    ui->noteTextEdit->setIgnoredClickUrlSchemata(
            QStringList() << "note" << "task");
    ui->encryptedNoteTextEdit->setIgnoredClickUrlSchemata(
            QStringList() << "note" << "task");

    // handle note url externally in the note text edit
    QObject::connect(
            ui->noteTextEdit,
            SIGNAL(urlClicked(QUrl)),
            this,
            SLOT(openLocalUrl(QUrl)));

    // also handle note url externally in the encrypted note text edit
    QObject::connect(
            ui->encryptedNoteTextEdit,
            SIGNAL(urlClicked(QUrl)),
            this,
            SLOT(openLocalUrl(QUrl)));

    // set the tab stop to the width of 4 spaces in the editor
    const int tabStop = 4;
    QFont font = ui->noteTextEdit->font();
    QFontMetrics metrics(font);
    int width = tabStop * metrics.width(' ');
    ui->noteTextEdit->setTabStopWidth(width);
    ui->encryptedNoteTextEdit->setTabStopWidth(width);

    // called now in readSettingsFromSettingsDialog() line 494
    // set the edit mode for the note text edit
    // this->setNoteTextEditMode(true);

    // load the note folder list in the menu
    this->loadNoteFolderListMenu();

    this->updateService = new UpdateService(this);
    this->updateService->checkForUpdates(this, UpdateService::AppStart);

    // update the current folder tooltip
    updateCurrentFolderTooltip();

    // add some different shortcuts for the note history on the mac
#ifdef Q_OS_MAC
    ui->action_Back_in_note_history->
            setShortcut(Qt::CTRL + Qt::ALT + Qt::Key_Left);
    ui->action_Forward_in_note_history->
            setShortcut(Qt::CTRL + Qt::ALT + Qt::Key_Right);
#endif

    // adding some alternate shortcuts for changing the current note
    QShortcut *shortcut = new QShortcut(QKeySequence("Ctrl+PgDown"), this);
    QObject::connect(shortcut, SIGNAL(activated()),
                     this, SLOT(on_actionNext_note_triggered()));
    shortcut = new QShortcut(QKeySequence("Ctrl+PgUp"), this);
    QObject::connect(shortcut, SIGNAL(activated()),
                     this, SLOT(on_actionPrevious_Note_triggered()));

    // show the app metrics notification if not already shown
    showAppMetricsNotificationIfNeeded();

    frequentPeriodicChecker();

    // setup the shortcuts for the note bookmarks
    setupNoteBookmarkShortcuts();

    // setup the markdown view
    setupMarkdownView();

    // setup the note edit pane
    setupNoteEditPane();

    // restore the distraction free mode
    restoreDistractionFreeMode();

    // add action tracking
    connect(ui->menuBar, SIGNAL(triggered(QAction *)),
            this, SLOT(trackAction(QAction *)));

    // set "show toolbar" menu item checked/unchecked
    const QSignalBlocker blocker(ui->actionShow_toolbar);
    {
        Q_UNUSED(blocker);
        ui->actionShow_toolbar->setChecked(!ui->mainToolBar->isHidden());
    }

    connect(ui->mainToolBar, SIGNAL(visibilityChanged(bool)),
            this, SLOT(mainToolbarVisibilityChanged(bool)));

    // set the action group for the width selector of the distraction free mode
    QActionGroup *dfmEditorWidthActionGroup = new QActionGroup(this);
    dfmEditorWidthActionGroup->addAction(ui->actionEditorWidthNarrow);
    dfmEditorWidthActionGroup->addAction(ui->actionEditorWidthMedium);
    dfmEditorWidthActionGroup->addAction(ui->actionEditorWidthWide);
    dfmEditorWidthActionGroup->addAction(ui->actionEditorWidthFull);
    dfmEditorWidthActionGroup->setExclusive(true);

    connect(dfmEditorWidthActionGroup, SIGNAL(triggered(QAction *)),
            this, SLOT(dfmEditorWidthActionTriggered(QAction *)));

    setAcceptDrops(true);
    // we need to disallow this explicitly under Windows
    // so that the MainWindow gets the event
    ui->noteTextEdit->setAcceptDrops(false);

    // do a bit more styling
    initStyling();
}

MainWindow::~MainWindow() {
    storeUpdatedNotesToDisk();
    delete ui;
}


/*!
 * Methods
 */

/**
 * Initializes the toolbars
 */
void MainWindow::initToolbars() {
    _formattingToolbar = new QToolBar(tr("formatting toolbar"), this);
    _formattingToolbar->addAction(ui->actionFormat_text_bold);
    _formattingToolbar->addAction(ui->actionFormat_text_italic);
    _formattingToolbar->addAction(ui->actionInset_code_block);
    _formattingToolbar->setObjectName("formattingToolbar");
    addToolBar(_formattingToolbar);

    _insertingToolbar = new QToolBar(tr("inserting toolbar"), this);
    _insertingToolbar->addAction(ui->actionInsert_Link_to_note);
    _insertingToolbar->addAction(ui->actionInsert_image);
    _insertingToolbar->addAction(ui->actionInsert_current_time);
    _insertingToolbar->setObjectName("insertingToolbar");
    addToolBar(_insertingToolbar);

    _encryptionToolbar = new QToolBar(tr("encryption toolbar"), this);
    _encryptionToolbar->addAction(ui->action_Encrypt_note);
    _encryptionToolbar->addAction(ui->actionEdit_encrypted_note);
    _encryptionToolbar->addAction(ui->actionDecrypt_note);
    _encryptionToolbar->setObjectName("encryptionToolbar");
    addToolBar(_encryptionToolbar);

    _windowToolbar =
            new QToolBar(tr("window toolbar"), this);
    _windowToolbar->addAction(ui->actionToggle_tag_pane);
    _windowToolbar->addAction(ui->actionToggle_note_edit_pane);
    _windowToolbar->addAction(ui->actionToggle_markdown_preview);
    _windowToolbar->addSeparator();
    _windowToolbar->addAction(
            ui->actionToggle_distraction_free_mode);
    _windowToolbar->addAction(ui->action_Increase_note_text_size);
    _windowToolbar->addAction(ui->action_Decrease_note_text_size);
    _windowToolbar->addAction(ui->action_Reset_note_text_size);
    _windowToolbar->setObjectName("windowToolbar");
    addToolBar(_windowToolbar);
}

/**
 * Restores the distraction free mode
 */
void MainWindow::restoreDistractionFreeMode() {
    if (isInDistractionFreeMode()) {
        setDistractionFreeMode(true);
    }
}

/**
 * Checks if we are in distraction free mode
 */
bool MainWindow::isInDistractionFreeMode() {
    QSettings settings;
    return settings.value("DistractionFreeMode/isEnabled").toBool();
}

/**
 * Toggles the distraction free mode
 */
void MainWindow::toggleDistractionFreeMode() {
    QSettings settings;
    bool isInDistractionFreeMode = this->isInDistractionFreeMode();

    qDebug() << __func__ << " - 'isInDistractionFreeMode': " <<
    isInDistractionFreeMode;

    // store the window settings before we go into distraction free mode
    if (!isInDistractionFreeMode) {
        storeSettings();
    }

    isInDistractionFreeMode = !isInDistractionFreeMode;

    // remember that we were using the distraction free mode
    settings.setValue("DistractionFreeMode/isEnabled",
                      isInDistractionFreeMode);

    setDistractionFreeMode(isInDistractionFreeMode);
}

/**
 * Does some basic styling
 */
void MainWindow::initStyling() {
    QPalette palette;
    QColor color = palette.color(QPalette::Base);

    QString textEditStyling = QString("QTextEdit {background-color: %1;}")
            .arg(color.name());

    ui->noteTextEdit->setStyleSheet(
            ui->noteTextEdit->styleSheet() + textEditStyling);

    ui->encryptedNoteTextEdit->setStyleSheet(
            ui->encryptedNoteTextEdit->styleSheet() + textEditStyling);

    QString frameStyling = QString("QFrame {background-color: %1;}")
            .arg(color.name());

    ui->noteTagFrame->setStyleSheet(
            ui->noteTextView->styleSheet() + frameStyling);

    if (!isInDistractionFreeMode()) {
        ui->noteTextEdit->setPaperMargins(0);
        ui->encryptedNoteTextEdit->setPaperMargins(0);
    }

#ifdef Q_OS_MAC
    // no stylesheets needed for OS X, the margins doesn't work the same there
    ui->tagFrame->setStyleSheet("");
    ui->notesListFrame->setStyleSheet("");
    ui->noteEditFrame->setStyleSheet("");
    ui->noteViewFrame->setStyleSheet("");
#endif

    // move the note view scrollbar when the note edit scrollbar was moved
    connect(ui->noteTextEdit->verticalScrollBar(), SIGNAL(valueChanged(int)),
            this, SLOT(noteTextSliderValueChanged(int)));
    connect(ui->encryptedNoteTextEdit->verticalScrollBar(),
            SIGNAL(valueChanged(int)),
            this, SLOT(noteTextSliderValueChanged(int)));

    // move the note edit scrollbar when the note view scrollbar was moved
    connect(ui->noteTextView->verticalScrollBar(),
            SIGNAL(valueChanged(int)),
            this, SLOT(noteViewSliderValueChanged(int)));
}

/**
 * Moves the note view scrollbar when the note edit scrollbar was moved
 */
void MainWindow::noteTextSliderValueChanged(int value) {
    // don't react if note text edit doesn't have the focus
    if (!activeNoteTextEdit()->hasFocus()) {
        return;
    }

    QScrollBar *editScrollBar = activeNoteTextEdit()->verticalScrollBar();
    QScrollBar *viewScrollBar = ui->noteTextView->verticalScrollBar();

    float editScrollFactor =
            static_cast<float>(value) / editScrollBar->maximum();
    int viewPosition =
            static_cast<int>(viewScrollBar->maximum() * editScrollFactor);

    // set the scroll position in the note text view
    viewScrollBar->setSliderPosition(viewPosition);
}

/**
 * Moves the note edit scrollbar when the note view scrollbar was moved
 */
void MainWindow::noteViewSliderValueChanged(int value) {
    // don't react if note text view doesn't have the focus
    if (!ui->noteTextView->hasFocus()) {
        return;
    }

    QScrollBar *editScrollBar = activeNoteTextEdit()->verticalScrollBar();
    QScrollBar *viewScrollBar = ui->noteTextView->verticalScrollBar();

    editScrollBar->maximum();

    float editScrollFactor =
            static_cast<float>(value) / viewScrollBar->maximum();

    int editPosition =
            static_cast<int>(editScrollBar->maximum() * editScrollFactor);

    // set the scroll position in the note text edit
    editScrollBar->setSliderPosition(editPosition);
}

/**
 * Enables or disables the distraction free mode
 */
void MainWindow::setDistractionFreeMode(bool enabled) {
    QSettings settings;

    if (enabled) {
        //
        // enter the distraction free mode
        //

        // remember states, geometry and sizes
        settings.setValue("DistractionFreeMode/windowState", saveState());
        settings.setValue("DistractionFreeMode/menuBarGeometry",
                          ui->menuBar->saveGeometry());
        settings.setValue("DistractionFreeMode/mainSplitterSizes",
                          mainSplitter->saveState());
        settings.setValue("DistractionFreeMode/menuBarHeight",
                          ui->menuBar->height());

        // we must not hide the menu bar or else the shortcuts
        // will not work any more
        ui->menuBar->setFixedHeight(0);

        // hide the toolbars
        ui->mainToolBar->hide();
        _formattingToolbar->hide();
        _insertingToolbar->hide();
        _encryptionToolbar->hide();
        _windowToolbar->hide();

        // hide the search line edit
        ui->searchLineEdit->hide();

        // hide tag frames if tagging is enabled
        if (isTagsEnabled()) {
            ui->tagFrame->hide();
            ui->noteTagFrame->hide();
        }

        // hide note view if markdown view is enabled
        if (isMarkdownViewEnabled()) {
            ui->noteViewFrame->hide();
        }

        // hide the status bar
//        ui->statusBar->hide();

        // hide the notes list widget
        ui->notesListFrame->hide();

//        QList<int> sizes = mainSplitter->sizes();
//        int size = sizes.takeFirst() + sizes.takeFirst();
//        sizes << 0 << size;
//        mainSplitter->setSizes(sizes);

        _leaveDistractionFreeModeButton = new QPushButton(tr("leave"));
        _leaveDistractionFreeModeButton->setFlat(true);
        _leaveDistractionFreeModeButton->setToolTip(
                tr("leave distraction free mode"));
        _leaveDistractionFreeModeButton
                ->setStyleSheet("QPushButton {padding: 0 5px}");

        _leaveDistractionFreeModeButton->setIcon(QIcon::fromTheme(
                "zoom-original",
                QIcon(":icons/breeze-qownnotes/16x16/zoom-original.svg")));

        connect(_leaveDistractionFreeModeButton, SIGNAL(clicked()),
                this, SLOT(toggleDistractionFreeMode()));

        statusBar()->addPermanentWidget(_leaveDistractionFreeModeButton);
    } else {
        //
        // leave the distraction free mode
        //

        statusBar()->removeWidget(_leaveDistractionFreeModeButton);
        disconnect(_leaveDistractionFreeModeButton, 0, 0, 0);

        // restore states and sizes
        QByteArray state = settings.value
                ("DistractionFreeMode/mainSplitterSizes").toByteArray();
        mainSplitter->restoreState(state);
        restoreState(
                settings.value(
                        "DistractionFreeMode/windowState").toByteArray());
        ui->menuBar->restoreGeometry(
                settings.value(
                        "DistractionFreeMode/menuBarGeometry").toByteArray());
        ui->menuBar->setFixedHeight(
                settings.value("DistractionFreeMode/menuBarHeight").toInt());

        // show the search line edit
        ui->searchLineEdit->show();

        ui->notesListFrame->show();

        // show tag frames if tagging is enabled
        if (isTagsEnabled()) {
            ui->tagFrame->show();
            ui->noteTagFrame->show();
        }

        // show note view if markdown view is enabled
        if (isMarkdownViewEnabled()) {
            ui->noteViewFrame->show();
        }
    }

    ui->noteTextEdit->setPaperMargins(this->width());
    ui->encryptedNoteTextEdit->setPaperMargins(this->width());
}

/**
 * Sets the distraction free mode if it is currently other than we want it to be
 */
void MainWindow::changeDistractionFreeMode(bool enabled) {
    if (isInDistractionFreeMode() != enabled) {
        setDistractionFreeMode(enabled);
    }
}

/**
 * Shows a status bar message if not in distraction free mode
 */
void MainWindow::showStatusBarMessage(const QString & message, int timeout) {
    if (!isInDistractionFreeMode()) {
        ui->statusBar->showMessage(message, timeout);
    }
}

/**
 * Sets the shortcuts for the note bookmarks up
 */
void MainWindow::setupNoteBookmarkShortcuts() {
    this->storeNoteBookmarkSignalMapper = new QSignalMapper(this);
    this->gotoNoteBookmarkSignalMapper = new QSignalMapper(this);

    for (int number = 0; number <= 9; number++) {
        // setup the store shortcut
        QShortcut *storeShortcut = new QShortcut(
                QKeySequence("Ctrl+Shift+" + QString::number(number)),
                this);

        connect(storeShortcut, SIGNAL(activated()),
                storeNoteBookmarkSignalMapper, SLOT(map()));
        storeNoteBookmarkSignalMapper->setMapping(storeShortcut, number);

        // setup the goto shortcut
        QShortcut *gotoShortcut = new QShortcut(
                QKeySequence("Ctrl+" + QString::number(number)),
                this);

        connect(gotoShortcut, SIGNAL(activated()),
                gotoNoteBookmarkSignalMapper, SLOT(map()));
        gotoNoteBookmarkSignalMapper->setMapping(gotoShortcut, number);
    }

    connect(storeNoteBookmarkSignalMapper, SIGNAL(mapped(int)),
            this, SLOT(storeNoteBookmark(int)));

    connect(gotoNoteBookmarkSignalMapper, SIGNAL(mapped(int)),
            this, SLOT(gotoNoteBookmark(int)));
}

/*
 * Loads the menu entries for the note folders
 */
void MainWindow::loadNoteFolderListMenu() {
    // clear menu list
    // we must not do this, because the app might crash if trackAction() is
    // called, because the action was triggered and then removed
//    ui->noteFoldersMenu->clear();

    // find all actions of the recent note folders menu
    QList<QAction*> actions =
            ui->noteFoldersMenu->findChildren<QAction*>();

    // loop through all actions of the recent note folders menu and hide them
    // this is a workaround because the app might crash if trackAction() is
    // called, because the action was triggered and then removed
    int c = 0;
    Q_FOREACH(QAction* action, actions) {
            // start with the 2nd item, the first item is the menu itself
            if (c++ > 0) {
                // hide menu item
                action->setVisible(false);
            }
        }

    QList<NoteFolder> noteFolders = NoteFolder::fetchAll();
    int noteFoldersCount = noteFolders.count();

    const QSignalBlocker blocker(ui->noteFolderComboBox);
    Q_UNUSED(blocker);

    ui->noteFolderComboBox->clear();
    int index = 0;
    int noteFolderComboBoxIndex = 0;

    // populate the note folder list
    if (noteFoldersCount > 0) {
        Q_FOREACH(NoteFolder noteFolder, noteFolders) {
                // don't show not existing folders or if path is empty
                if (!noteFolder.localPathExists()) {
                    continue;
                }

                // add an entry to the combo box
                ui->noteFolderComboBox->addItem(noteFolder.getName(),
                                                      noteFolder.getId());

                // add a menu entry
                QAction *action =
                        ui->noteFoldersMenu->addAction(noteFolder.getName());
                action->setData(noteFolder.getId());
                action->setToolTip(noteFolder.getLocalPath());
                action->setStatusTip(noteFolder.getLocalPath());

                if (noteFolder.isCurrent()) {
                    QFont font = action->font();
                    font.setBold(true);
                    action->setFont(font);

                    noteFolderComboBoxIndex = index;
                }

                QObject::connect(
                        action, SIGNAL(triggered()),
                        recentNoteFolderSignalMapper, SLOT(map()));

                // add a parameter to changeNoteFolder with the signal mapper
                recentNoteFolderSignalMapper->setMapping(
                        action, noteFolder.getId());

                index++;
            }

        QObject::connect(recentNoteFolderSignalMapper,
                         SIGNAL(mapped(int)),
                         this,
                         SLOT(changeNoteFolder(int)));

        // set the current row
        ui->noteFolderComboBox->setCurrentIndex(
                noteFolderComboBoxIndex);
    }
}

/*
 * Set a new note folder
 */
void MainWindow::changeNoteFolder(int noteFolderId, bool forceChange) {
    NoteFolder noteFolder = NoteFolder::fetch(noteFolderId);
    if (!noteFolder.isFetched()) {
        return;
    }

    if (noteFolder.isCurrent() && !forceChange) {
        return;
    }

    QString folderName = noteFolder.getLocalPath();
    QString oldPath = this->notesPath;

    // reload notes if notes folder was changed
    if (oldPath != folderName) {
        // store everything before changing folder
        storeUpdatedNotesToDisk();

        noteFolder.setAsCurrent();

        // update the recent note folder list
        storeRecentNoteFolder(this->notesPath, folderName);

        // change notes path
        this->notesPath = folderName;

        // store notesPath setting
        QSettings settings;
        settings.setValue("notesPath", folderName);

        // we have to unset the current note otherwise it might show up after
        // switching to an other note folder
        currentNote = Note();

        buildNotesIndex();
        loadNoteDirectoryList();

        const QSignalBlocker blocker(this->ui->noteTextEdit);
        {
            Q_UNUSED(blocker);
            ui->noteTextEdit->clear();
            ui->noteTextEdit->show();
            ui->encryptedNoteTextEdit->hide();
        }

        const QSignalBlocker blocker2(this->ui->searchLineEdit);
        {
            Q_UNUSED(blocker2);
            ui->searchLineEdit->clear();
        }

        this->ui->noteTextView->clear();

        // update the current folder tooltip
        updateCurrentFolderTooltip();

        // clear the note history
        this->noteHistory.clear();
    }
}

/*
 * Adds and removes a folder from the recent note folders
 */
void MainWindow::storeRecentNoteFolder(
        QString addFolderName,
        QString removeFolderName) {
    QSettings settings;
    QStringList recentNoteFolders =
            settings.value("recentNoteFolders").toStringList();

    recentNoteFolders.removeAll(addFolderName);
    recentNoteFolders.removeAll(removeFolderName);

    // remove empty paths
    recentNoteFolders.removeAll("");

    if (addFolderName != removeFolderName) {
        recentNoteFolders.prepend(addFolderName);
    }

    settings.setValue("recentNoteFolders", recentNoteFolders);
    // reload menu
    loadNoteFolderListMenu();
}

int MainWindow::openNoteDiffDialog(Note changedNote) {
    if (this->noteDiffDialog->isVisible()) {
        this->noteDiffDialog->close();
    }

    QString text1 = this->ui->noteTextEdit->toPlainText();

    changedNote.updateNoteTextFromDisk();
    QString text2 = changedNote.getNoteText();

//    qDebug() << __func__ << " - 'text1': " << text1;
//    qDebug() << __func__ << " - 'text2': " << text2;

    diff_match_patch *diff = new diff_match_patch();
    QList<Diff> diffList = diff->diff_main(text1, text2);

    QString html = diff->diff_prettyHtml(diffList);
//    qDebug() << __func__ << " - 'html': " << html;

    this->noteDiffDialog = new NoteDiffDialog(this, html);
    this->noteDiffDialog->exec();

    int result = this->noteDiffDialog->resultActionRole();
    return result;
}

/**
 * Does the initialization for the main splitter
 */
void MainWindow::initMainSplitter() {
    mainSplitter = new QSplitter();
    mainSplitter->setHandleWidth(0);

    ui->tagFrame->setStyleSheet("#tagFrame {margin-right: 3px;}");
    ui->notesListFrame->setStyleSheet("#notesListFrame {margin: 0;}");

    _verticalNoteFrame = new QFrame();
    _verticalNoteFrame->setObjectName("verticalNoteFrame");
    _verticalNoteFrame->setStyleSheet(
            "#verticalNoteFrame {margin: 0 0 0 3px;}");
    _verticalNoteFrame->setFrameShape(QFrame::NoFrame);
    _verticalNoteFrame->setVisible(false);

    _verticalNoteFrameSplitter = new QSplitter(Qt::Vertical);
    _verticalNoteFrameSplitter->setHandleWidth(0);

    QVBoxLayout *layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(_verticalNoteFrameSplitter);
    _verticalNoteFrame->setLayout(layout);

    mainSplitter->addWidget(ui->tagFrame);
    mainSplitter->addWidget(ui->notesListFrame);
    mainSplitter->addWidget(_verticalNoteFrame);

    // restore main splitter state
    QSettings settings;
    QByteArray state = settings.value("mainSplitterSizes").toByteArray();
    mainSplitter->restoreState(state);

    ui->centralWidget->layout()->addWidget(this->mainSplitter);

    // do the further setup for the main splitter and all the panes
    setupMainSplitter();

    // setup the checkbox
    const QSignalBlocker blocker(ui->actionUse_vertical_preview_layout);
    Q_UNUSED(blocker);
    ui->actionUse_vertical_preview_layout
            ->setChecked(isVerticalPreviewModeEnabled());
}

/**
 * Does the further setup for the main splitter and all the panes
 */
void MainWindow::setupMainSplitter() {
    if ( isVerticalPreviewModeEnabled() ) {
        ui->noteEditFrame->setStyleSheet("#noteEditFrame {margin: 0 0 3px 0;}");
        ui->noteViewFrame->setStyleSheet("#noteViewFrame {margin: 0;}");

        _verticalNoteFrameSplitter->addWidget(ui->noteEditFrame);
        _verticalNoteFrameSplitter->addWidget(ui->noteViewFrame);

        // disable collapsing for all widgets in the splitter, users had
        // problems with collapsed panels
        for (int i = 0; i < _verticalNoteFrameSplitter->count(); i++) {
            _verticalNoteFrameSplitter->setCollapsible(i, false);
        }

        // restore the vertical note frame splitter state
        QSettings settings;
        _verticalNoteFrameSplitter->restoreState(settings.value(
                "verticalNoteFrameSplitterState").toByteArray());
    } else {
        ui->noteEditFrame->setStyleSheet("#noteEditFrame {margin: 0 0 0 3px;}");
        ui->noteViewFrame->setStyleSheet("#noteViewFrame {margin: 0 0 0 3px;}");

        mainSplitter->addWidget(ui->noteEditFrame);
        mainSplitter->addWidget(ui->noteViewFrame);
    }

    // disable collapsing for all widgets in the splitter, users had problems
    // with collapsed panels
    for (int i = 0; i < mainSplitter->count(); i++) {
        mainSplitter->setCollapsible(i, false);
    }

    // set the visibillity of the vertical note frame
    _verticalNoteFrame->setVisible(isVerticalPreviewModeEnabled() &&
                (isNoteEditPaneEnabled() || isMarkdownViewEnabled()));
}

void MainWindow::createSystemTrayIcon() {
    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(QIcon(":/images/icon.png"));
    connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(
                    systemTrayIconClicked(QSystemTrayIcon::ActivationReason)));
    if (showSystemTray) {
        trayIcon->show();
    }
}

void MainWindow::loadNoteDirectoryList() {
    {
        const QSignalBlocker blocker(ui->noteTextEdit);
        Q_UNUSED(blocker);

        {
            const QSignalBlocker blocker2(ui->notesListWidget);
            Q_UNUSED(blocker2);

            ui->notesListWidget->clear();

            // load all notes and add them to the note list widget
            QList<Note> noteList = Note::fetchAll();
            Q_FOREACH(Note note, noteList) {
                    QString name = note.getName();

                    // skip notes without name
                    if (name.isEmpty()) {
                        continue;
                    }

                    QListWidgetItem *item = new QListWidgetItem(name);
                    setListWidgetItemToolTipForNote(item, &note);
                    item->setIcon(QIcon::fromTheme(
                            "text-x-generic",
                            QIcon(":icons/breeze-qownnotes/16x16/"
                                          "text-x-generic.svg")));
                    item->setData(Qt::UserRole, note.getId());
                    ui->notesListWidget->addItem(item);
            }

            // clear the text edits if there are no notes
            if (noteList.isEmpty()) {
                ui->noteTextEdit->clear();
                ui->noteTextView->clear();
            }

            int itemCount = noteList.count();
            MetricsService::instance()->sendEventIfEnabled(
                    "note/list/loaded",
                    "note",
                    "note list loaded",
                    QString::number(itemCount) + " notes",
                    itemCount);
        }
    }

    QDir dir(this->notesPath);

    // clear all paths from the directory watcher
    QStringList fileList = noteDirectoryWatcher.directories() +
            noteDirectoryWatcher.files();
    if (fileList.count() > 0) {
        noteDirectoryWatcher.removePaths(fileList);
    }

    if (dir.exists()) {
        // watch the notes directory for changes
        this->noteDirectoryWatcher.addPath(this->notesPath);
    }

    QStringList fileNameList = Note::fetchNoteFileNames();

    // watch all the notes for changes
    int count = 0;
    Q_FOREACH(QString fileName, fileNameList) {
#ifdef Q_OS_LINUX
            // only add the last first 200 notes to the file watcher to
            // prevent that nothing is watched at all because of too many
            // open files
            if (count > 200) {
                break;
            }
#endif

            QString path = Note::getFullNoteFilePathForFile(fileName);
            QFile file(path);
            if (file.exists()) {
                this->noteDirectoryWatcher.addPath(path);
                count++;
            }
    }

    // sort alphabetically again if necessary
    if (sortAlphabetically) {
        ui->notesListWidget->sortItems(Qt::AscendingOrder);
    }

    // setup tagging
    setupTags();
}

/**
 * Sets the list widget tooltip for a note
 */
void MainWindow::setListWidgetItemToolTipForNote(
        QListWidgetItem *item,
        Note *note,
        QDateTime *overrideFileLastModified) {
    if ((item == NULL) || (note == NULL)) {
        return;
    }

    QDateTime modified = note->getFileLastModified();
    QDateTime *fileLastModified = (overrideFileLastModified != NULL) ?
                                 overrideFileLastModified : &modified;

    item->setToolTip(tr("<strong>%1</strong><br />last modified: %2")
            .arg(note->getName(), fileLastModified->toString()));
}

/**
 * @brief makes the current note the first item in the note list without reloading the whole list
 */
void MainWindow::makeCurrentNoteFirstInNoteList() {
    QString name = this->currentNote.getName();
    QList<QListWidgetItem *> items =
            this->ui->notesListWidget->findItems(name, Qt::MatchExactly);
    if (items.count() > 0) {
        const QSignalBlocker blocker(this->ui->notesListWidget);
        Q_UNUSED(blocker);

        ui->notesListWidget->takeItem(ui->notesListWidget->row(items[0]));
        ui->notesListWidget->insertItem(0, items[0]);
        this->ui->notesListWidget->setCurrentItem(items[0]);
    }
}

void MainWindow::readSettings() {
    NoteFolder::migrateToNoteFolders();

    QSettings settings;
    sortAlphabetically = settings.value(
            "SortingModeAlphabetically", false).toBool();
    showSystemTray = settings.value("ShowSystemTray", false).toBool();
    restoreGeometry(settings.value("MainWindow/geometry").toByteArray());
    restoreState(settings.value("MainWindow/windowState").toByteArray());
    ui->menuBar->restoreGeometry(
            settings.value("MainWindow/menuBarGeometry").toByteArray());

    // read all relevant settings, that can be set in the settings dialog
    readSettingsFromSettingsDialog();

    // get notes path
    this->notesPath = settings.value("notesPath").toString();

    // migration: remove GAnalytics-cid
    if (!settings.value("GAnalytics-cid").toString().isEmpty()) {
        settings.remove("GAnalytics-cid");
    }

    // let us select a folder if we haven't find one in the settings
    if (this->notesPath == "") {
        selectOwnCloudNotesFolder();
    }

    // migration: remove notes path from recent note folders
    if (this->notesPath != "") {
        QStringList recentNoteFolders =
                settings.value("recentNoteFolders").toStringList();
        if (recentNoteFolders.contains(this->notesPath)) {
            recentNoteFolders.removeAll(this->notesPath);
            settings.setValue("recentNoteFolders", recentNoteFolders);
        }
    }

    // set the editor width selector for the distraction free mode
    int editorWidthMode =
            settings.value("DistractionFreeMode/editorWidthMode").toInt();

    switch (editorWidthMode) {
        case QOwnNotesMarkdownTextEdit::Medium:
            ui->actionEditorWidthMedium->setChecked(true);
            break;
        case QOwnNotesMarkdownTextEdit::Wide:
            ui->actionEditorWidthWide->setChecked(true);
            break;
        case QOwnNotesMarkdownTextEdit::Full:
            ui->actionEditorWidthFull->setChecked(true);
            break;
        default:
        case QOwnNotesMarkdownTextEdit::Narrow:
            ui->actionEditorWidthNarrow->setChecked(true);
            break;
    }
}

/**
 * @brief Reads all relevant settings, that can be set in the settings dialog
 */
void MainWindow::readSettingsFromSettingsDialog() {
    QSettings settings;

    // disable the automatic update dialog per default for repositories and
    // self-builds
    if (settings.value("disableAutomaticUpdateDialog").toString().isEmpty()) {
        QString release = QString(RELEASE);
        bool enabled =
                release.contains("Travis") || release.contains("AppVeyor");
        settings.setValue("disableAutomaticUpdateDialog", !enabled);
    }

    this->notifyAllExternalModifications =
            settings.value("notifyAllExternalModifications").toBool();
    this->noteSaveIntervalTime = settings.value("noteSaveIntervalTime").toInt();

    // default value is 10 seconds
    if (this->noteSaveIntervalTime == 0) {
        this->noteSaveIntervalTime = 10;
        settings.setValue("noteSaveIntervalTime", this->noteSaveIntervalTime);
    }

    // set the note text edit font
    ui->noteTextEdit->setStyles();
    ui->encryptedNoteTextEdit->setStyles();

    // load note text view font
    QString fontString = settings.value("MainWindow/noteTextView.font")
            .toString();

    // store the current font if there isn't any set yet
    if (fontString == "") {
        fontString = ui->noteTextView->font().toString();
        settings.setValue("MainWindow/noteTextView.font", fontString);
    }

    // set the note text view font
    QFont font;
    font.fromString(fontString);
    ui->noteTextView->setFont(font);

    // set the main toolbar icon size
    int toolBarIconSize = settings.value(
            "MainWindow/mainToolBar.iconSize").toInt();
    if (toolBarIconSize == 0) {
        toolBarIconSize = ui->mainToolBar->iconSize().height();
        settings.setValue(
                "MainWindow/mainToolBar.iconSize",
                QString::number(toolBarIconSize));
    } else {
        QSize size(toolBarIconSize, toolBarIconSize);
        ui->mainToolBar->setIconSize(size);
        _formattingToolbar->setIconSize(size);
        _insertingToolbar->setIconSize(size);
        _encryptionToolbar->setIconSize(size);
        _windowToolbar->setIconSize(size);
    }

    // check if we want to view the note folder combo box
    ui->noteFolderComboBox->setVisible(
            settings.value(
                    "MainWindow/showRecentNoteFolderInMainArea").toBool());

    // change the search notes symbol between dark and light mode
    QString fileName = settings.value("darkModeColors").toBool() ?
                       "search-notes-dark.svg" : "search-notes.svg";
    QString styleSheet = ui->searchLineEdit->styleSheet();
    styleSheet.replace(
            QRegularExpression("background-image: url\\(:.+\\);"),
            QString("background-image: url(:/images/%1);").arg(fileName));
    ui->searchLineEdit->setStyleSheet(styleSheet);
}

void MainWindow::updateNoteTextFromDisk(Note note) {
    note.updateNoteTextFromDisk();
    note.store();
    this->currentNote = note;
    updateEncryptNoteButtons();

    {
        const QSignalBlocker blocker(this->ui->noteTextEdit);
        Q_UNUSED(blocker);
        this->setNoteTextFromNote(&note);
    }
}

void MainWindow::notesWereModified(const QString &str) {
    qDebug() << "notesWereModified: " << str;

    QFileInfo fi(str);
    Note note = Note::fetchByFileName(fi.fileName());

    // load note from disk if current note was changed
    if (note.getFileName() == this->currentNote.getFileName()) {
        if (note.fileExists()) {
            // fetch current text
            QString text1 = this->ui->noteTextEdit->toPlainText();

            // fetch text of note from disk
            note.updateNoteTextFromDisk();
            QString text2 = note.getNoteText();

            // skip dialog if texts are equal
            if (text1 == text2) {
                return;
            }

            qDebug() << "Current note was modified externally!";

            showStatusBarMessage(
                    tr("current note was modified externally"), 3000);

            // if we don't want to get notifications at all
            // external modifications check if we really need one
            if (!this->notifyAllExternalModifications) {
                bool isCurrentNoteNotEditedForAWhile =
                        this->currentNoteLastEdited.addSecs(60)
                        < QDateTime::currentDateTime();

                // reloading the current note text straight away
                // if we didn't change it for a minute
                if (!this->currentNote.getHasDirtyData()
                    && isCurrentNoteNotEditedForAWhile) {
                    updateNoteTextFromDisk(note);
                    return;
                }
            }

            int result = openNoteDiffDialog(note);
            switch (result) {
                // overwrite file with local changes
                case NoteDiffDialog::Overwrite: {
                    const QSignalBlocker blocker(this->noteDirectoryWatcher);
                    Q_UNUSED(blocker);
                    this->currentNote.store();
                    this->currentNote.storeNoteTextFileToDisk();
                    showStatusBarMessage(
                            tr("stored current note to disk"), 1000);

                    // just to make sure everything is uptodate
//                        this->currentNote = note;
//                        this->setNoteTextFromNote( &note, true );

                    // wait 100ms before the block on this->noteDirectoryWatcher
                    // is opened, otherwise we get the event
                    waitMsecs(100);
                }
                    break;

                // reload note file from disk
                case NoteDiffDialog::Reload:
                    updateNoteTextFromDisk(note);
                    break;

                case NoteDiffDialog::Cancel:
                case NoteDiffDialog::Ignore:
                default:
                    // do nothing
                    break;
            }
        } else {
            qDebug() << "Current note was removed externally!";

            switch (QMessageBox::information(
                    this, tr("Note was removed externally!"),
                    tr("Current note was removed outside of this application!\n"
                            "Restore current note?"),
                     tr("&Restore"), tr("&Cancel"), QString::null,
                                             0, 1)) {
                case 0: {
                    const QSignalBlocker blocker(this->noteDirectoryWatcher);
                    Q_UNUSED(blocker);

                    QString text = this->ui->noteTextEdit->toPlainText();
                    note.storeNewText(text);

                    // store note to disk again
                    note.storeNoteTextFileToDisk();
                    showStatusBarMessage(
                            tr("stored current note to disk"), 1000);

                    // rebuild and reload the notes directory list
                    buildNotesIndex();
                    loadNoteDirectoryList();

                    // fetch note new (because all the IDs have changed
                    // after the buildNotesIndex()
                    note.refetch();

                    // restore old selected row (but don't update the note text)
                    setCurrentNote(note, false);
                }
                    break;
                case 1:
                default:
                    break;
            }
        }
    } else {
        qDebug() << "other note was changed: " << str;

        showStatusBarMessage(
                tr("note was modified externally: %1").arg(str), 3000);

        // rebuild and reload the notes directory list
        buildNotesIndex();
        loadNoteDirectoryList();
        setCurrentNote(this->currentNote, false);
    }
}

void MainWindow::notesDirectoryWasModified(const QString &str) {
    qDebug() << "notesDirectoryWasModified: " << str;
    showStatusBarMessage(tr("notes directory was modified externally"), 3000);

    // rebuild and reload the notes directory list
    buildNotesIndex();
    loadNoteDirectoryList();

    // also update the text of the text edit if current note has changed
    bool updateNoteText = !this->currentNote.exists();
    qDebug() << "updateNoteText: " << updateNoteText;

    // restore old selected row (but don't update the note text)
    setCurrentNote(this->currentNote, updateNoteText);
}

/**
 * Checks if the note view needs an update because the text has changed
 */
void MainWindow::noteViewUpdateTimerSlot() {
    if (_noteViewNeedsUpdate) {
        if (isMarkdownViewEnabled()) {
            setNoteTextFromNote(&currentNote, true);
        }
        _noteViewNeedsUpdate = false;
    }
}

void MainWindow::storeUpdatedNotesToDisk() {
    {
        const QSignalBlocker blocker(this->noteDirectoryWatcher);
        Q_UNUSED(blocker);

        QString oldNoteName = this->currentNote.getName();

        // For some reason this->noteDirectoryWatcher gets an event from this.
        // I didn't find an other solution than to wait yet.
        // All flushing and syncing didn't help.
        int count = Note::storeDirtyNotesToDisk(this->currentNote);

        if (count > 0) {
            _noteViewNeedsUpdate = true;

            MetricsService::instance()
                    ->sendEventIfEnabled(
                            "note/notes/stored",
                            "note",
                            "notes stored",
                            QString::number(count) + " notes",
                            count);

            qDebug() << __func__ << " - 'count': " << count;

            showStatusBarMessage(
                    tr("stored %n note(s) to disk", "", count),
                    1000);

            // wait 100ms before the block on this->noteDirectoryWatcher
            // is opened, otherwise we get the event
            waitMsecs(100);

            // just to make sure everything is uptodate
            this->currentNote.refetch();

            QString newNoteName = this->currentNote.getName();
            if (oldNoteName == newNoteName) {
                if ( !sortAlphabetically ) {
                    // if note name has not changed makes the current note
                    // the first item in the note list without
                    // reloading the whole list
                    makeCurrentNoteFirstInNoteList();
                }
            } else {
                // rename the note file names of note tag links
                Tag::renameNoteFileNamesOfLinks(oldNoteName, newNoteName);

                // reload the directory list if note name has changed
                loadNoteDirectoryList();
            }
        }
    }
}

/**
 * Shows alerts for calendar items with an alarm date in the current minute
 * Also checks for expired note crypto keys
 */
void MainWindow::frequentPeriodicChecker() {
    CalendarItem::alertTodoReminders();
    Note::expireCryptoKeys();
    MetricsService::instance()->sendHeartbeat();

    QSettings settings;
    QDateTime lastUpdateCheck = settings.value("LastUpdateCheck").toDateTime();
    if (!lastUpdateCheck.isValid()) {
        // set the LastUpdateCheck if it wasn't set
        settings.setValue("LastUpdateCheck", QDateTime::currentDateTime());
    } else if (lastUpdateCheck.addSecs(3600) <= QDateTime::currentDateTime()) {
        // check for updates every 1h
        updateService->checkForUpdates(this, UpdateService::Periodic);
    }
}

/**
 * Does the setup for the update available button
 */
void MainWindow::setupUpdateAvailableButton() {
    _updateAvailableButton = new QPushButton(this);
    _updateAvailableButton->setFlat(true);
    _updateAvailableButton->setToolTip(
            tr("click here to see what has changed and to be able to "
                       "download the latest version"));
    _updateAvailableButton->hide();
    _updateAvailableButton->setStyleSheet("QPushButton {padding: 0 5px}");

    QObject::connect(
            _updateAvailableButton,
            SIGNAL(pressed()),
            this,
            SLOT(on_actionCheck_for_updates_triggered()));

    ui->statusBar->addPermanentWidget(_updateAvailableButton);
}

void MainWindow::showUpdateAvailableButton(QString version) {
    _updateAvailableButton->setText(
            tr("new version %1 available").arg(version));
    _updateAvailableButton->show();
}

void MainWindow::hideUpdateAvailableButton() {
    _updateAvailableButton->hide();
}

void MainWindow::waitMsecs(int msecs) {
    QTime dieTime = QTime::currentTime().addMSecs(msecs);
    while (QTime::currentTime() < dieTime)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

void MainWindow::buildNotesIndex() {
    // make sure we destroy nothing
    storeUpdatedNotesToDisk();

    QDir notesDir(this->notesPath);

    // only show markdown and text files
    QStringList filters;
    filters << "*.txt" << "*.md";

    // show newest entry first
    QStringList files = notesDir.entryList(filters, QDir::Files, QDir::Time);
    qDebug() << __func__ << " - 'files': " << files;

    bool createDemoNotes = files.count() == 0;

    if (createDemoNotes) {
        QSettings settings;
        // check if we already have created the demo notes once
        createDemoNotes = !settings.value("demoNotesCreated").toBool();

        if (createDemoNotes) {
            // we don't want to create the demo notes again
            settings.setValue("demoNotesCreated", true);
        }
    }

    // add some notes if there aren't any and
    // we haven't already created them once
    if (createDemoNotes) {
        qDebug() << "No notes! We will add some...";
        QStringList filenames = QStringList() <<
                "Markdown Showcase.md" <<
                "GitHub Flavored Markdown.md" <<
                "Welcome to QOwnNotes.md";
        QString filename;
        QString destinationFile;

        // copy note files to the notes path
        for (int i = 0; i < filenames.size(); ++i) {
            filename = filenames.at(i);
            destinationFile = this->notesPath + QDir::separator() + filename;
            QFile sourceFile(":/demonotes/" + filename);
            sourceFile.copy(destinationFile);
            // set read/write permissions for the owner and user
            QFile::setPermissions(destinationFile,
                                  QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ReadUser | QFile::WriteUser);
        }

        // copy the shortcuts file and handle its file permissions
//        destinationFile = this->notesPath + QDir::separator() +
//              "Important Shortcuts.txt";
//        QFile::copy( ":/shortcuts", destinationFile );
//        QFile::setPermissions( destinationFile, QFile::ReadOwner |
//                  QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser );

        // fetch all files again
        files = notesDir.entryList(filters, QDir::Files, QDir::Time);

        // jump to the welcome note in the note selector in 500ms
        QTimer::singleShot(500, this, SLOT(jumpToWelcomeNote()));
    }

    // get the current crypto key to set it again
    // after all notes were read again
    qint64 cryptoKey = currentNote.getCryptoKey();
    QString cryptoPassword = currentNote.getCryptoPassword();

    // delete all notes in the database first
    Note::deleteAll();

    // create all notes from the files
    Q_FOREACH(QString fileName, files) {
            // fetching the content of the file
            QFile file(Note::getFullNoteFilePathForFile(fileName));
            Note note;
            note.createFromFile(file);
        }

    // re-fetch current note (because all the IDs have changed after the
    // buildNotesIndex()
    currentNote.refetch();

    if (cryptoKey != 0) {
        // reset the old crypto key for the current note
        currentNote.setCryptoKey(cryptoKey);
        currentNote.setCryptoPassword(cryptoPassword);
        currentNote.store();
    }

    // setup the note folder database
    DatabaseService::createNoteFolderConnection();
    DatabaseService::setupNoteFolderTables();
}

/**
 * @brief Jumps to the welcome note in the note selector
 */
void MainWindow::jumpToWelcomeNote() {
    // search for the welcome note
    QList<QListWidgetItem *> items = ui->notesListWidget->
            findItems("Welcome to QOwnNotes", Qt::MatchExactly);
    if (items.count() > 0) {
        // set the welcome note as current note
        ui->notesListWidget->setCurrentItem(items.at(0));
    }
}

QString MainWindow::selectOwnCloudNotesFolder() {
    QString path = this->notesPath;

    if (path == "") {
        path = QDir::homePath() + QDir::separator() +
                "ownCloud" + QDir::separator() + "Notes";
    }

    // TODO(pbek): We sometimes seem to get a "QCoreApplication::postEvent:
    // Unexpected null receiver" here.
    QString dir = QFileDialog::getExistingDirectory(
            this,
            tr("Please select the folder where your notes will get stored to"),
            path,
            QFileDialog::ShowDirsOnly);

    QDir d = QDir(dir);

    if (d.exists() && (dir != "")) {
        // let's remove trailing slashes
        dir = d.path();

        // update the recent note folder list
        storeRecentNoteFolder(this->notesPath, dir);

        this->notesPath = dir;
        QSettings settings;
        settings.setValue("notesPath", dir);

        // update the current folder tooltip
        updateCurrentFolderTooltip();
    } else {
        if (this->notesPath == "") {
            switch (QMessageBox::information(
                   this, tr("No folder was selected"),
                    tr("You have to select your ownCloud notes "
                            "folder to make this software work!"),
                    tr("&Retry"), tr("&Exit"), QString::null,
                    0, 1)) {
                case 0:
                    selectOwnCloudNotesFolder();
                    break;
                case 1:
                default:
                    // No other way to quit the application worked
                    // in the constructor
                    QTimer::singleShot(0, this, SLOT(quitApp()));
                    QTimer::singleShot(100, this, SLOT(quitApp()));
                    break;
            }
        }
    }

    return this->notesPath;
}

void MainWindow::setCurrentNote(Note note,
                                bool updateNoteText,
                                bool updateSelectedNote,
                                bool addNoteToHistory) {
    MetricsService::instance()->sendVisitIfEnabled("note/current-note/changed");

    enableShowVersionsButton();
    enableShowTrashButton();

    // update cursor position of previous note
    if (this->currentNote.exists()) {
        QTextCursor c = ui->noteTextEdit->textCursor();
        this->noteHistory.updateCursorPositionOfNote(
                this->currentNote, c.position());
    }

    // add new note to history
    if (addNoteToHistory && note.exists()) {
        this->noteHistory.add(note);
    }

    this->currentNote = note;
    QString name = note.getName();
    this->setWindowTitle(name + " - QOwnNotes " + QString(VERSION));

    // set the note text edit to readonly if note file is not writable
    QFileInfo *f = new QFileInfo(
            this->notesPath + QDir::separator() + note.getFileName());
    ui->noteTextEdit->setReadOnly(!f->isWritable());
    ui->encryptedNoteTextEdit->setReadOnly(!f->isWritable());

    // find and set the current item
    if (updateSelectedNote) {
        QList<QListWidgetItem *> items = this->ui->notesListWidget->findItems(
                name, Qt::MatchExactly);
        if (items.count() > 0) {
            const QSignalBlocker blocker(this->ui->notesListWidget);
            Q_UNUSED(blocker);

            this->ui->notesListWidget->setCurrentItem(items[0]);
        }
    }

    // update the text of the text edit
    if (updateNoteText) {
        const QSignalBlocker blocker(this->ui->noteTextEdit);
        Q_UNUSED(blocker);

        this->setNoteTextFromNote(&note);

        // hide the encrypted note text edit by default and show the regular one
        ui->encryptedNoteTextEdit->hide();
        ui->noteTextEdit->show();
    }

    updateEncryptNoteButtons();
    reloadCurrentNoteTags();
}

void MainWindow::focusNoteTextEdit() {
    // move the cursor to the 4nd line
    QTextCursor tmpCursor = ui->noteTextEdit->textCursor();
    tmpCursor.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor);
    tmpCursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
    tmpCursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
    tmpCursor.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor);
    ui->noteTextEdit->setTextCursor(tmpCursor);

    // focus note text edit
    ui->noteTextEdit->setFocus();
}

void MainWindow::removeCurrentNote() {
    // store updated notes to disk
    storeUpdatedNotesToDisk();

    switch (QMessageBox::information(this, tr("Remove current note"),
                     tr("Remove current note: <strong>%1</strong>?")
                             .arg(this->currentNote.getName()),
                     tr("&Remove"), tr("&Cancel"), QString::null,
                     0, 1)) {
        case 0: {
            QList<QListWidgetItem*> noteList =
                    ui->notesListWidget->findItems(currentNote.getName(),
                                           Qt::MatchExactly);

            if (noteList.count() > 0) {
                const QSignalBlocker blocker1(ui->notesListWidget);
                Q_UNUSED(blocker1);

                const QSignalBlocker blocker2(ui->noteTextEdit);
                Q_UNUSED(blocker2);

                const QSignalBlocker blocker3(ui->noteTextView);
                Q_UNUSED(blocker3);

                const QSignalBlocker blocker4(ui->encryptedNoteTextEdit);
                Q_UNUSED(blocker4);

                const QSignalBlocker blocker5(noteDirectoryWatcher);
                Q_UNUSED(blocker5);

                // delete note in database and on file system
                currentNote.remove(true);

                ui->noteTextEdit->clear();
                ui->noteTextView->clear();
                ui->encryptedNoteTextEdit->clear();

                // delete item in note list widget
                delete noteList[0];

                // set a new first note
                resetCurrentNote();
            }

            break;
        }
        case 1:
        default:
            break;
    }
}

/**
 * Resets the current note to the first note
 */
void MainWindow::resetCurrentNote() {
    // set new current note
    if (ui->notesListWidget->count() > 0) {
        const QSignalBlocker blocker(ui->notesListWidget);
        Q_UNUSED(blocker);

        ui->notesListWidget->setCurrentRow(0);

        Note note = Note::fetchByName(
                ui->notesListWidget->currentItem()->text());
        setCurrentNote(note, true, false);
    }
}

void MainWindow::storeSettings() {
    QSettings settings;

    // don't store the window settings in distraction free mode
    if (!isInDistractionFreeMode()) {
        settings.setValue("MainWindow/geometry", saveGeometry());
        settings.setValue("MainWindow/windowState", saveState());
        settings.setValue("mainSplitterSizes", mainSplitter->saveState());
        settings.setValue("verticalNoteFrameSplitterState",
                          _verticalNoteFrameSplitter->saveState());
        settings.setValue("MainWindow/menuBarGeometry",
                          ui->menuBar->saveGeometry());
    }

    settings.setValue("SortingModeAlphabetically", sortAlphabetically);
    settings.setValue("ShowSystemTray", showSystemTray);
}


/*!
 * Internal events
 */

void MainWindow::closeEvent(QCloseEvent *event) {
    MetricsService::instance()->sendVisitIfEnabled("app/end", "app end");
    storeSettings();
    QMainWindow::closeEvent(event);
}

//
// Event filters on the MainWindow
//
bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);

        if (obj == ui->searchLineEdit) {
            // set focus to the notes list if Key_Down or Key_Tab were
            // pressed in the search line edit
            if ((keyEvent->key() == Qt::Key_Down) ||
                (keyEvent->key() == Qt::Key_Tab)) {
                // choose an other selected item if current item is invisible
                QListWidgetItem *item = ui->notesListWidget->currentItem();
                if ((item != NULL) &&
                    ui->notesListWidget->currentItem()->isHidden() &&
                    (this->firstVisibleNoteListRow >= 0)) {
                    ui->notesListWidget->setCurrentRow(
                            this->firstVisibleNoteListRow);
                }

                // give the keyboard focus to the notes list widget
                ui->notesListWidget->setFocus();
                return true;
            }
            return false;
        } else if (obj == activeNoteTextEdit()) {
            // check if we want to leave the distraction free mode and the
            // search widget is not visible (because we want to close that
            // first)
            if ((keyEvent->key() == Qt::Key_Escape)
                && isInDistractionFreeMode()
                && !activeNoteTextEdit()->searchWidget()->isVisible()) {
                toggleDistractionFreeMode();
                return false;
            }
            return false;
        } else if (obj == ui->notesListWidget) {
            // set focus to the note text edit if Key_Return or Key_Tab were
            // pressed in the notes list
            if ((keyEvent->key() == Qt::Key_Return) ||
                    (keyEvent->key() == Qt::Key_Tab)) {
                focusNoteTextEdit();
                return true;
            } else if ((keyEvent->key() == Qt::Key_Delete) ||
                       (keyEvent->key() == Qt::Key_Backspace)) {
                removeSelectedNotes();
                return true;
            }
            return false;
        } else if (obj == ui->tagListWidget) {
            if ((keyEvent->key() == Qt::Key_Delete) ||
                (keyEvent->key() == Qt::Key_Backspace)) {
                removeSelectedTags();
                return true;
            }
            return false;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);

        if ((mouseEvent->button() == Qt::BackButton)) {
            // move back in the note history
            on_action_Back_in_note_history_triggered();
        } else if ((mouseEvent->button() == Qt::ForwardButton)) {
            // move forward in the note history
            on_action_Forward_in_note_history_triggered();
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

/**
 * highlights all occurrences of str in the note text edit
 */
void MainWindow::searchInNoteTextEdit(QString &str) {
    QList<QTextEdit::ExtraSelection> extraSelections;
    QList<QTextEdit::ExtraSelection> extraSelections2;
    QList<QTextEdit::ExtraSelection> extraSelections3;

    if (str.count() >= 2) {
        ui->noteTextEdit->moveCursor(QTextCursor::Start);
        ui->noteTextView->moveCursor(QTextCursor::Start);
        ui->encryptedNoteTextEdit->moveCursor(QTextCursor::Start);
        QColor color = QColor(0, 180, 0, 100);

        while (ui->noteTextEdit->find(str)) {
            QTextEdit::ExtraSelection extra;
            extra.format.setBackground(color);

            extra.cursor = ui->noteTextEdit->textCursor();
            extraSelections.append(extra);
        }

        while (ui->noteTextView->find(str)) {
            QTextEdit::ExtraSelection extra;
            extra.format.setBackground(color);

            extra.cursor = ui->noteTextView->textCursor();
            extraSelections2.append(extra);
        }

        while (ui->encryptedNoteTextEdit->find(str)) {
            QTextEdit::ExtraSelection extra;
            extra.format.setBackground(color);

            extra.cursor = ui->encryptedNoteTextEdit->textCursor();
            extraSelections3.append(extra);
        }
    }

    ui->noteTextEdit->setExtraSelections(extraSelections);
    ui->noteTextView->setExtraSelections(extraSelections2);
    ui->encryptedNoteTextEdit->setExtraSelections(extraSelections3);
}

/**
 * highlights all occurrences of tje search line text in the note text edit
 */
void MainWindow::searchForSearchLineTextInNoteTextEdit() {
    QString searchString = ui->searchLineEdit->text();
    searchInNoteTextEdit(searchString);
}

/**
 * Asks for the password if the note is encrypted and can't be decrypted
 */
void MainWindow::askForEncryptedNotePasswordIfNeeded(QString additionalText) {
    currentNote.refetch();

    // check if the note is encrypted and can't be decrypted
    if (currentNote.hasEncryptedNoteText() &&
        !currentNote.canDecryptNoteText()) {
        QString labelText =
                tr("Please enter the <strong>password</strong> "
                        "of this encrypted note.");

        if (!additionalText.isEmpty()) {
            labelText += " " + additionalText;
        }

        PasswordDialog* dialog = new PasswordDialog(this, labelText);
        int dialogResult = dialog->exec();

        // if user pressed ok take the password
        if (dialogResult == QDialog::Accepted) {
            QString password = dialog->password();
            if (password != "") {
                // set the password so it can be decrypted
                // for the markdown view
                currentNote.setCryptoPassword(password);
                currentNote.store();
            }

            // warn if password is incorrect
            if (!currentNote.canDecryptNoteText()) {
                QMessageBox::warning(
                        this,
                        tr("Note can't be decrypted!"),
                        tr("It seems that your password is not valid!"));
            }
        }
    }
}

/**
 * Sets the note text according to a note
 */
void MainWindow::setNoteTextFromNote(Note *note, bool updateNoteTextViewOnly) {
    if (!updateNoteTextViewOnly) {
        this->ui->noteTextEdit->setText(note->getNoteText());
    }

    this->ui->noteTextView->setHtml(note->toMarkdownHtml(notesPath));

    // update the slider when editing notes
    noteTextSliderValueChanged(
            activeNoteTextEdit()->verticalScrollBar()->value());
}

/**
 * Sets the text of the current note.
 * This is a public callback function for the version dialog.
 *
 * @brief MainWindow::setCurrentNoteText
 * @param text
 */
void MainWindow::setCurrentNoteText(QString text) {
    currentNote.setNoteText(text);
    setNoteTextFromNote(&currentNote, false);
}

/**
 * Creates a new note (to restore a trashed note)
 * This is a public callback function for the trash dialog.
 *
 * @brief MainWindow::createNewNote
 * @param name
 * @param text
 */
void MainWindow::createNewNote(QString name, QString text) {
    QString extension = Note::defaultNoteFileExtension();
    QFile *f = new QFile(this->notesPath + QDir::separator() + name + "." + extension);

    // change the name and headline if note exists
    if (f->exists()) {
        QDateTime currentDate = QDateTime::currentDateTime();
        name.append(" " + currentDate.toString(Qt::ISODate).replace(":", "."));

        QString preText = name + "\n";

        for (int i = 0; i < name.length(); i++) {
            preText.append("=");
        }

        preText.append("\n\n");
        text.prepend(preText);
    }

    ui->searchLineEdit->setText(name);
    on_searchLineEdit_returnPressed();
    ui->noteTextEdit->setText(text);
}

/**
 * @brief Restores a trashed note on the server.
 * @param name
 * @param text
 *
 * This is a public callback function for the trash dialog.
 */
void MainWindow::restoreTrashedNoteOnServer(QString fileName, int timestamp) {
    OwnCloudService *ownCloud = new OwnCloudService(this);
    ownCloud->restoreTrashedNoteOnServer(
            this->notesPath, fileName, timestamp, this);
}

/**
 * @brief Removes selected notes after a confirmation
 */
void MainWindow::removeSelectedNotes() {
    // store updated notes to disk
    storeUpdatedNotesToDisk();

    int selectedItemsCount = ui->notesListWidget->selectedItems().size();

    if (selectedItemsCount == 0) {
        return;
    }

    if (QMessageBox::information(
            this,
            tr("Remove selected notes"),
            tr("Remove <strong>%n</strong> selected note(s)?\n\n"
               "If the trash is enabled on your "
                    "ownCloud server you should be able to restore "
                    "them from there.",
               "", selectedItemsCount),
             tr("&Remove"), tr("&Cancel"), QString::null,
             0, 1) == 0) {
        const QSignalBlocker blocker(this->noteDirectoryWatcher);
        Q_UNUSED(blocker);

        const QSignalBlocker blocker1(ui->notesListWidget);
        Q_UNUSED(blocker1);

        const QSignalBlocker blocker2(ui->noteTextEdit);
        Q_UNUSED(blocker2);

        const QSignalBlocker blocker3(ui->noteTextView);
        Q_UNUSED(blocker3);

        const QSignalBlocker blocker4(ui->encryptedNoteTextEdit);
        Q_UNUSED(blocker4);

        Q_FOREACH(QListWidgetItem *item, ui->notesListWidget->selectedItems()) {
            QString name = item->text();
            Note note = Note::fetchByName(name);
            note.remove(true);
            qDebug() << "Removed note " << name;
        }

        loadNoteDirectoryList();

        // set a new first note
        resetCurrentNote();
    }
}

/**
 * @brief Removes selected tags after a confirmation
 */
void MainWindow::removeSelectedTags() {
    int selectedItemsCount = ui->tagListWidget->selectedItems().size();

    if (selectedItemsCount == 0) {
        return;
    }

    if (QMessageBox::information(
            this,
            tr("Remove selected tags"),
            tr("Remove <strong>%n</strong> selected tag(s)? No notes will "
                       "be removed in this process.",
               "", selectedItemsCount),
             tr("&Remove"), tr("&Cancel"), QString::null,
             0, 1) == 0) {
        const QSignalBlocker blocker(this->noteDirectoryWatcher);
        Q_UNUSED(blocker);

        const QSignalBlocker blocker1(ui->tagListWidget);
        Q_UNUSED(blocker1);

        Q_FOREACH(QListWidgetItem *item, ui->tagListWidget->selectedItems()) {
            int tagId = item->data(Qt::UserRole).toInt();
            Tag tag = Tag::fetch(tagId);
            tag.remove();
            qDebug() << "Removed tag " << tag.getName();
        }

        reloadTagList();
    }
}

/**
 * @brief Select all notes
 */
void MainWindow::selectAllNotes() {
    ui->notesListWidget->selectAll();
}

/**
 * @brief Moves selected notes after a confirmation
 * @param destinationFolder
 */
void MainWindow::moveSelectedNotesToFolder(QString destinationFolder) {
    // store updated notes to disk
    storeUpdatedNotesToDisk();

    int selectedItemsCount = ui->notesListWidget->selectedItems().size();

    if (QMessageBox::information(
            this,
            tr("Move selected notes"),
            tr("Move %n selected note(s) to <strong>%2</strong>?", "",
               selectedItemsCount).arg(destinationFolder),
            tr("&Move"), tr("&Cancel"), QString::null,
            0, 1) == 0) {
        const QSignalBlocker blocker(this->noteDirectoryWatcher);
        Q_UNUSED(blocker);

        Q_FOREACH(QListWidgetItem *item, ui->notesListWidget->selectedItems()) {
                QString name = item->text();
                Note note = Note::fetchByName(name);

                // remove note path form directory watcher
                this->noteDirectoryWatcher.removePath(note.fullNoteFilePath());

                if (note.getId() == currentNote.getId()) {
                    // reset the current note
                    this->currentNote = Note();

                    // clear the note text edit
                    const QSignalBlocker blocker2(ui->noteTextEdit);
                    Q_UNUSED(blocker2);
                    ui->noteTextEdit->clear();

                    // clear the encrypted note text edit
                    const QSignalBlocker blocker3(ui->encryptedNoteTextEdit);
                    Q_UNUSED(blocker3);
                    ui->encryptedNoteTextEdit->clear();
                }

                // move note
                bool result = note.move(destinationFolder);
                if (result) {
                    qDebug() << "Note was moved:" << note.getName();
                } else {
                    qWarning() << "Could not move note:" << note.getName();
                }
            }

        loadNoteDirectoryList();
    }
}

/**
 * @brief Copies selected notes after a confirmation
 * @param destinationFolder
 */
void MainWindow::copySelectedNotesToFolder(QString destinationFolder) {
    int selectedItemsCount = ui->notesListWidget->selectedItems().size();

    if (QMessageBox::information(
            this,
            tr("Copy selected notes"),
            tr("Copy %n selected note(s) to <strong>%2</strong>?", "",
               selectedItemsCount).arg(destinationFolder),
            tr("&Copy"), tr("&Cancel"), QString::null, 0, 1) == 0) {
        int copyCount = 0;
        Q_FOREACH(QListWidgetItem *item, ui->notesListWidget->selectedItems()) {
                QString name = item->text();
                Note note = Note::fetchByName(name);

                // copy note
                bool result = note.copy(destinationFolder);
                if (result) {
                    copyCount++;
                    qDebug() << "Note was copied:" << note.getName();
                } else {
                    qWarning() << "Could not copy note:" << note.getName();
                }
            }

        QMessageBox::information(
                this, tr("Done"),
                tr("%n note(s) were copied to <strong>%2</strong>.", "",
                   copyCount).arg(destinationFolder));
    }
}

/**
 * Tags selected notes
 */
void MainWindow::tagSelectedNotes(Tag tag) {
    int selectedItemsCount = ui->notesListWidget->selectedItems().size();

    if (QMessageBox::information(
            this,
            tr("Tag selected notes"),
            tr("Tag %n selected note(s) with <strong>%2</strong>?", "",
               selectedItemsCount).arg(tag.getName()),
            tr("&Tag"), tr("&Cancel"), QString::null, 0, 1) == 0) {
        int tagCount = 0;
        Q_FOREACH(QListWidgetItem *item, ui->notesListWidget->selectedItems()) {
                QString name = item->text();
                Note note = Note::fetchByName(name);

                // tag note
                bool result = tag.linkToNote(note);
                if (result) {
                    tagCount++;
                    qDebug() << "Note was tagged:" << note.getName();
                } else {
                    qWarning() << "Could not tag note:" << note.getName();
                }
            }

        QMessageBox::information(
                this, tr("Done"),
                tr("%n note(s) were tagged with <strong>%2</strong>.", "",
                   tagCount).arg(tag.getName()));
    }
}

/**
 * Removes a tag from the selected notes
 */
void MainWindow::removeTagFromSelectedNotes(Tag tag) {
    int selectedItemsCount = ui->notesListWidget->selectedItems().size();

    if (QMessageBox::information(
            this,
            tr("Remove tag from selected notes"),
            tr("Remove tag <strong>%1</strong> from %n selected note(s)?", "",
               selectedItemsCount).arg(tag.getName()),
            tr("&Remove"), tr("&Cancel"), QString::null, 0, 1) == 0) {
        int tagCount = 0;
        Q_FOREACH(QListWidgetItem *item, ui->notesListWidget->selectedItems()) {
                QString name = item->text();
                Note note = Note::fetchByName(name);

                // tag note
                bool result = tag.removeLinkToNote(note);
                if (result) {
                    tagCount++;
                    qDebug() << "Tag was removed from note:" << note.getName();
                } else {
                    qWarning() << "Could not remove tag from note:"
                    << note.getName();
                }
            }

        QMessageBox::information(
                this, tr("Done"),
                tr("Tag <strong>%1</strong> was removed from %n note(s)", "",
                   tagCount).arg(tag.getName()));
    }
}

/**
 * @brief Updates the current folder tooltip
 */
void MainWindow::updateCurrentFolderTooltip() {
    ui->actionSet_ownCloud_Folder
            ->setStatusTip(tr("Current notes folder: ") + this->notesPath);
    ui->actionSet_ownCloud_Folder
            ->setToolTip(tr("Set the notes folder. Current notes folder: ") +
                                 this->notesPath);
}

/**
 * @brief Opens the settings dialog
 */
void MainWindow::openSettingsDialog(int tab) {
    int currentNoteFolderId = NoteFolder::currentNoteFolderId();

    // open the settings dialog
    SettingsDialog *dialog = new SettingsDialog(tab, this);
    int dialogResult = dialog->exec();

    if (dialogResult == QDialog::Accepted) {
        // read all relevant settings, that can be set in the settings dialog
        readSettingsFromSettingsDialog();

        // reset the note save timer
        this->noteSaveTimer->stop();
        this->noteSaveTimer->start(this->noteSaveIntervalTime * 1000);
    }

    // if the current note folder was changed we will change the note path
    if (currentNoteFolderId != NoteFolder::currentNoteFolderId()) {
        NoteFolder noteFolder = NoteFolder::currentNoteFolder();

        if (noteFolder.isFetched()) {
            changeNoteFolder(noteFolder.getId(), true);
        }
    }

    // reload note folders in case we changed them in the settings
    loadNoteFolderListMenu();
}

/**
 * @brief Returns the active note text edit
 */
QMarkdownTextEdit* MainWindow::activeNoteTextEdit() {
    return ui->noteTextEdit->isHidden() ?
                                  ui->encryptedNoteTextEdit : ui->noteTextEdit;
}

/**
 * @brief Handles the linking of text
 */
void MainWindow::handleTextNoteLinking() {
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    LinkDialog *dialog = new LinkDialog(tr("Link to an url or note"), this);
    dialog->exec();
    if (dialog->result() == QDialog::Accepted) {
        QString url = dialog->getURL();
        QString noteName = dialog->getSelectedNoteName();
        QString noteNameForLink = Note::generateTextForLink(noteName);

        if ((noteName != "") || (url != "")) {
            QString selectedText =
                    textEdit->textCursor().selectedText();
            QString newText;

            // if user has entered an url
            if (url != "") {
                if (selectedText != "") {
                    newText = "[" + selectedText + "](" + url + ")";
                } else {
                    // if possible fetch the title of the webpage
                    QString title = dialog->getTitleForUrl(QUrl(url));

                    // if we got back a tile let's use it in the link
                    if (title != "") {
                        newText = "[" + title + "](" + url + ")";
                    } else {
                        newText = "<" + url + ">";
                    }
                }
            } else {
                // if user has selected a note
                if (selectedText != "") {
                    newText = "[" + selectedText + "]"
                           "(note://" + noteNameForLink + ")";
                } else {
                    newText = "<note://" + noteNameForLink + ">";
                }
            }
            textEdit->textCursor().insertText(newText);
        }
    }
}

/**
 * Downloads an url and stores it to a file
 */
bool MainWindow::downloadUrlToFile(QUrl url, QFile *file) {
    if (!file->open(QIODevice::WriteOnly)) {
        return false;
    }

    if (!file->isWritable()) {
        return false;
    }

    QNetworkAccessManager *manager = new QNetworkAccessManager(this);
    QEventLoop loop;
    QTimer timer;

    timer.setSingleShot(true);
    connect(&timer, SIGNAL(timeout()), &loop, SLOT(quit()));
    connect(manager, SIGNAL(finished(QNetworkReply *)), &loop, SLOT(quit()));

    // 10 sec timeout for the request
    timer.start(10000);

    QNetworkReply *reply = manager->get(QNetworkRequest(url));
    loop.exec();

    // if we didn't get a timeout let's write the file
    if (timer.isActive()) {
        // get the text from the network reply
        QByteArray data = reply->readAll();
        if (data.size() > 0) {
            file->write(data);
            return true;
        }
    }

    // timer elapsed, no reply from network request or empty data
    return false;
}


/**
 * @brief Sets the current note from a CurrentNoteHistoryItem
 * @param item
 */
void MainWindow::setCurrentNoteFromHistoryItem(NoteHistoryItem item) {
    qDebug() << item;
    qDebug() << item.getNote();

    setCurrentNote(item.getNote(), true, true, false);
    QTextCursor c = ui->noteTextEdit->textCursor();
    c.setPosition(item.getCursorPosition());
    ui->noteTextEdit->setTextCursor(c);
}

/**
 * @brief Prints the content of a text edit widget
 * @param textEdit
 */
void MainWindow::printNote(QTextEdit *textEdit) {
    QPrinter printer;

    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print note"));

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    textEdit->document()->print(&printer);
}

/**
 * @brief Exports the content of a text edit widget as PDF
 * @param textEdit
 */
void MainWindow::exportNoteAsPDF(QTextEdit *textEdit) {
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(QDir::homePath());
    dialog.setNameFilter(tr("PDF files (*.pdf)"));
    dialog.setWindowTitle(tr("Export current note as PDF"));
    dialog.selectFile(currentNote.getName() + ".pdf");
    int ret = dialog.exec();

    if (ret == QDialog::Accepted) {
        QStringList fileNames = dialog.selectedFiles();
        if (fileNames.count() > 0) {
            QString fileName = fileNames.at(0);

            if (QFileInfo(fileName).suffix().isEmpty()) {
                fileName.append(".pdf");
            }

            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(fileName);
            textEdit->document()->print(&printer);
        }
    }
}

/**
 * Shows the app metrics notification if not already shown
 */
void MainWindow::showAppMetricsNotificationIfNeeded() {
    QSettings settings;
    bool showDialog = !settings.value("appMetrics/notificationShown").toBool();

    if (showDialog) {
        settings.setValue("appMetrics/notificationShown", true);

        if (QMessageBox::information(
                this,
                "QOwnNotes",
                tr("QOwnNotes will track anonymous usage data, that helps to "
                        "decide what parts of QOwnNotes to improve next "
                        "and to find and fix bugs. You can disable that "
                        "behaviour in the settings."),
                tr("&Ok"),
                tr("Open &settings"),
                QString::null, 0, 1) == 1) {
            openSettingsDialog(SettingsDialog::GeneralTab);
        }
    }
}

/**
 * Opens the todo list dialog
 */
void MainWindow::openTodoDialog(QString taskUid) {
    QSettings settings;
    QStringList todoCalendarEnabledUrlList =
            settings.value("ownCloud/todoCalendarEnabledUrlList")
                    .toStringList();

    // check if we have got any todo list enabled
    if (todoCalendarEnabledUrlList.count() == 0) {
        if (QMessageBox::warning(
                0, tr("No selected todo lists!"),
                tr("You have not selected any todo lists.<br />"
                           "Please check your <strong>Todo</strong>"
                           "configuration in the settings!"),
                tr("Open &settings"),
                tr("&Cancel"),
                QString::null, 0, 1) == 0) {
            openSettingsDialog(SettingsDialog::TodoTab);
        }

        return;
    }

    TodoDialog *dialog = new TodoDialog(this, taskUid, this);
    dialog->exec();
}




// *****************************************************************************
// *
// *
// * Slot implementations
// *
// *
// *****************************************************************************

void MainWindow::on_notesListWidget_currentItemChanged(
        QListWidgetItem *current, QListWidgetItem *previous) {
    Q_UNUSED(previous);

    // in case all notes were removed
    if (current == NULL) {
        return;
    }

    qDebug() << "currentItemChanged " << current->text();

    Note note = Note::fetchByName(current->text());
    setCurrentNote(note, true, false);

    // parse the current note for markdown highlighting
    ui->noteTextEdit->highlighter()->parse();

    // let's highlight the text from the search line edit
    searchForSearchLineTextInNoteTextEdit();

    // also do a "in note search" if the widget is visible
    if (ui->noteTextEdit->searchWidget()->isVisible()) {
        ui->noteTextEdit->searchWidget()->doSearchDown();
    }
}

void MainWindow::on_noteTextEdit_textChanged() {
    Note note = this->currentNote;
    note.updateNoteTextFromDisk();
    QString noteTextFromDisk = note.getNoteText();

    QString text = this->ui->noteTextEdit->toPlainText();

    if (text != noteTextFromDisk) {
        this->currentNote.storeNewText(text);
        this->currentNote.refetch();
        this->currentNoteLastEdited = QDateTime::currentDateTime();
        _noteViewNeedsUpdate = true;

        updateEncryptNoteButtons();

        // update the note list tooltip of the note
        setListWidgetItemToolTipForNote(ui->notesListWidget->currentItem(),
                                        &currentNote,
                                        &currentNoteLastEdited);
    }
}

void MainWindow::on_action_Quit_triggered() {
    storeSettings();
    QApplication::quit();
}

void MainWindow::quitApp() {
    QApplication::quit();
}

void MainWindow::on_actionSet_ownCloud_Folder_triggered() {
    // store updated notes to disk
    storeUpdatedNotesToDisk();

    openSettingsDialog(SettingsDialog::NoteFolderTab);
}

void MainWindow::on_searchLineEdit_textChanged(const QString &arg1) {
    Q_UNUSED(arg1);
    filterNotes();
}

/**
 * Does the note filtering
 */
void MainWindow::filterNotes(bool searchForText) {
    // filter the notes by text in the search line edit
    filterNotesBySearchLineEditText();

    if (isTagsEnabled()) {
        // filter the notes by tag
        filterNotesByTag();
    }

    if (searchForText) {
        // let's highlight the text from the search line edit
        searchForSearchLineTextInNoteTextEdit();
    }
}

/**
 * Checks if the vertical preview mode is enabled
 */
bool MainWindow::isVerticalPreviewModeEnabled() {
    QSettings settings;
    return settings.value("verticalPreviewModeEnabled", false).toBool();
}

/**
 * Checks if tagging is enabled
 */
bool MainWindow::isTagsEnabled() {
    QSettings settings;
    return settings.value("tagsEnabled", false).toBool();
}

/**
 * Checks if the markdown view is enabled
 */
bool MainWindow::isMarkdownViewEnabled() {
    QSettings settings;
    return settings.value("markdownViewEnabled", true).toBool();
}

/**
 * Checks if the note edit pane is enabled
 */
bool MainWindow::isNoteEditPaneEnabled() {
    QSettings settings;
    return settings.value("noteEditPaneEnabled", true).toBool();
}

/**
 * Does the note filtering by text in the search line edit
 */
void MainWindow::filterNotesBySearchLineEditText() {
    QString arg1 = ui->searchLineEdit->text();

    // search notes when at least 2 characters were entered
    if (arg1.count() >= 2) {
        QList<QString> noteNameList = Note::searchAsNameList(arg1);
        this->firstVisibleNoteListRow = -1;

        for (int i = 0; i < this->ui->notesListWidget->count(); ++i) {
            QListWidgetItem *item = this->ui->notesListWidget->item(i);
            if (noteNameList.indexOf(item->text()) < 0) {
                item->setHidden(true);
            } else {
                if (this->firstVisibleNoteListRow < 0) {
                    this->firstVisibleNoteListRow = i;
                }
                item->setHidden(false);
            }
        }
    } else {
        // show all items otherwise
        this->firstVisibleNoteListRow = 0;

        for (int i = 0; i < this->ui->notesListWidget->count(); ++i) {
            QListWidgetItem *item = this->ui->notesListWidget->item(i);
            item->setHidden(false);
        }
    }
}

/**
 * Does the note filtering by tags
 */
void MainWindow::filterNotesByTag() {
    // check if there is an active tag
    Tag tag = Tag::activeTag();

    qDebug() << __func__ << " - 'tag': " << tag;

    if (!tag.isFetched()) {
        return;
    }

    // fetch all linked note names
    QStringList fileNameList = tag.fetchAllLinkedNoteFileNames();

    qDebug() << __func__ << " - 'fileNameList': " << fileNameList;


    // loop through all notes
    for (int i = 0; i < this->ui->notesListWidget->count(); ++i) {
        QListWidgetItem *item = this->ui->notesListWidget->item(i);
        // omit the already hidden notes
        if (item->isHidden()) {
            continue;
        }

        // hide all notes that are not linked to the active tag
        if (!fileNameList.contains(item->text())) {
            item->setHidden(true);
        } else {
            if (this->firstVisibleNoteListRow < 0) {
                this->firstVisibleNoteListRow = i;
            }
            item->setHidden(false);
        }
    }
}

//
// set focus on search line edit if Ctrl + Shift + F was pressed
//
void MainWindow::on_action_Find_note_triggered() {
    changeDistractionFreeMode(false);
    this->ui->searchLineEdit->setFocus();
    this->ui->searchLineEdit->selectAll();
}

//
// jump to found note or create a new one if not found
//
void MainWindow::on_searchLineEdit_returnPressed() {
    QString text = this->ui->searchLineEdit->text();
    text = text.trimmed();

    // first let us search for the entered text
    Note note = Note::fetchByName(text);

    // if we can't find a note we create a new one
    if (note.getId() == 0) {
        // create a headline in new notes by adding "=====" as second line
        QString noteText = text + "\n";
        for (int i = 0; i < text.length(); i++) {
            noteText.append("=");
        }
        noteText.append("\n\n");

        note = Note();
        note.setName(text);
        note.setNoteText(noteText);
        note.store();

        // store the note to disk
        {
            const QSignalBlocker blocker(this->noteDirectoryWatcher);
            Q_UNUSED(blocker);

            note.storeNoteTextFileToDisk();
            showStatusBarMessage(
                    tr("stored current note to disk"), 1000);
        }

        buildNotesIndex();
        loadNoteDirectoryList();

        // fetch note new (because all the IDs have changed after
        // the buildNotesIndex()
        note.refetch();

//        // create a new widget item for the note list
//        QListWidgetItem* widgetItem = new QListWidgetItem();
//        widgetItem->setText( text );

//        // insert the note at the top of the note list
//        {
//            const QSignalBlocker blocker( this->ui->notesListWidget );

//            ui->notesListWidget->insertItem( 0, widgetItem );
//        }
    }

    // jump to the found or created note
    setCurrentNote(note);

    // focus the note text edit and set the cursor correctly
    focusNoteTextEdit();
}

void MainWindow::on_action_Remove_note_triggered() {
    removeCurrentNote();
}

void MainWindow::on_actionAbout_QOwnNotes_triggered() {
    AboutDialog *dialog = new AboutDialog(this);
    dialog->exec();
}

//
// hotkey to create new note with date in name
//
void MainWindow::on_action_Note_note_triggered() {
    QDateTime currentDate = QDateTime::currentDateTime();

    // replacing ":" with "_" for Windows systems
    QString text =
            "Note " + currentDate.toString(Qt::ISODate).replace(":", ".");
    this->ui->searchLineEdit->setText(text);
    on_searchLineEdit_returnPressed();
}

/*
 * Handles urls in the noteTextView
 *
 * examples:
 * - <note://MyNote> opens the note "MyNote"
 * - <note://my-note-with-spaces-in-the-name> opens the note "My Note with spaces in the name"
 * - <http://www.qownnotes.org> opens the web page
 * - <file:///path/to/my/file/QOwnNotes.pdf> opens the file "/path/to/my/file/QOwnNotes.pdf" if the operating system supports that handler
 */
void MainWindow::on_noteTextView_anchorClicked(const QUrl &url) {
    qDebug() << __func__ << " - 'url': " << url;
    QString scheme = url.scheme();

    if ((scheme == "note" || scheme == "task")) {
        openLocalUrl(url);
    } else {
        ui->noteTextEdit->openUrl(url);
    }
}

/*
 * Handles note urls
 *
 * examples:
 * - <note://MyNote> opens the note "MyNote"
 * - <note://my-note-with-spaces-in-the-name> opens the note "My Note with spaces in the name"
 */
void MainWindow::openLocalUrl(QUrl url) {
    qDebug() << __func__ << " - 'url': " << url;
    QString scheme = url.scheme();

    if (scheme == "note") {
        // add a ".com" to the filename to simulate a valid domain
        QString fileName = url.host() + ".com";;

        // convert the ACE to IDN (internationalized domain names) to support
        // links to notes with unicode characters in their names
        // then remove the ".com" again
        fileName = Utils::Misc::removeIfEndsWith(
                QUrl::fromAce(fileName.toLatin1()), ".com");

        // if it seem we have unicode characters in our filename let us use
        // wildcards for each number, because full width numbers get somehow
        // translated to normal numbers by the QTextEdit
        if (fileName != url.host()) {
            fileName.replace("1", "[1１]")
                    .replace("2", "[2２]")
                    .replace("3", "[3３]")
                    .replace("4", "[4４]")
                    .replace("5", "[5５]")
                    .replace("6", "[6６]")
                    .replace("7", "[7７]")
                    .replace("8", "[8８]")
                    .replace("9", "[9９]")
                    .replace("0", "[0０]");
        }

        // this makes it possible to search for file names containing spaces
        // instead of spaces a "-" has to be used in the note link
        // example: note://my-note-with-spaces-in-the-name
        fileName.replace("-", "?").replace("_", "?");

        // we need to search for the case sensitive filename,
        // we only get it lowercase by QUrl
        QDir currentDir = QDir(this->notesPath);
        QStringList files;
        QStringList fileSearchList =
                QStringList() << fileName + ".txt" << fileName + ".md";

        // search for files with that name
        files = currentDir.entryList(fileSearchList,
                                     QDir::Files | QDir::NoSymLinks);

        // did we find files?
        if (files.length() > 0) {
            // take the first found file
            fileName = files.first();

            // try to fetch note
            Note note = Note::fetchByFileName(fileName);

            // does this note really exist?
            if (note.isFetched()) {
                // set current note
                setCurrentNote(note);
            }
        }
    } else if (scheme == "task") {
        openTodoDialog(url.host());
    }
}

/*
 * Manually check for updates
 */
void MainWindow::on_actionCheck_for_updates_triggered() {
    this->updateService->checkForUpdates(this, UpdateService::Manual);
}

/*
 * Open the issue page
 */
void MainWindow::on_actionReport_problems_or_ideas_triggered() {
    QDesktopServices::openUrl(QUrl("https://github.com/pbek/QOwnNotes/issues"));
}

void MainWindow::on_actionAlphabetical_triggered(bool checked) {
    if (checked) {
        sortAlphabetically = true;
        ui->notesListWidget->sortItems(Qt::AscendingOrder);
    }
}

void MainWindow::on_actionBy_date_triggered(bool checked) {
    if (checked) {
        sortAlphabetically = false;
        loadNoteDirectoryList();
    }
}

void MainWindow::systemTrayIconClicked(
        QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger) {
        if (this->isVisible()) {
            this->hide();
        } else {
            this->show();
        }
    }
}

void MainWindow::on_actionShow_system_tray_triggered(bool checked) {
    showSystemTray = checked;
    if (checked) {
        trayIcon->show();
    } else {
        trayIcon->hide();
    }
}

void MainWindow::on_action_Settings_triggered() {
    // open the settings dialog
    openSettingsDialog();
}

void MainWindow::on_actionShow_versions_triggered() {
    ui->actionShow_versions->setDisabled(true);
    showStatusBarMessage(
            tr("note versions are currently loaded from your ownCloud server"),
            20000);

    OwnCloudService *ownCloud = new OwnCloudService(this);
    ownCloud->loadVersions(this->currentNote.getFileName(), this);
}

void MainWindow::enableShowVersionsButton() {
    ui->actionShow_versions->setDisabled(false);
}

void MainWindow::on_actionShow_trash_triggered() {
    ui->actionShow_trash->setDisabled(true);
    showStatusBarMessage(
            tr("trashed notes are currently loaded from your ownCloud server"),
            20000);

    OwnCloudService *ownCloud = new OwnCloudService(this);
    ownCloud->loadTrash(this);
}

void MainWindow::enableShowTrashButton() {
    ui->actionShow_trash->setDisabled(false);
}

void MainWindow::on_notesListWidget_customContextMenuRequested(
        const QPoint &pos) {
    QPoint globalPos = ui->notesListWidget->mapToGlobal(pos);
    QMenu noteMenu;
    QMenu *moveDestinationMenu = new QMenu();
    QMenu *copyDestinationMenu = new QMenu();
    QMenu *tagMenu = new QMenu();
    QMenu *tagRemoveMenu = new QMenu();

    QList<NoteFolder> noteFolders = NoteFolder::fetchAll();

    // show copy and move menu entries only if there
    // is at least one other note folder
    if (noteFolders.count() > 1) {
        moveDestinationMenu = noteMenu.addMenu(tr("&Move notes to..."));
        copyDestinationMenu = noteMenu.addMenu(tr("&Copy notes to..."));

        Q_FOREACH(NoteFolder noteFolder, noteFolders) {
                // don't show not existing folders or if path is empty
                if (!noteFolder.localPathExists()) {
                    continue;
                }

                if (noteFolder.isCurrent()) {
                    continue;
                }

                QAction *moveAction = moveDestinationMenu->addAction(
                        noteFolder.getName());
                moveAction->setData(noteFolder.getLocalPath());
                moveAction->setToolTip(noteFolder.getLocalPath());
                moveAction->setStatusTip(noteFolder.getLocalPath());

                QAction *copyAction = copyDestinationMenu->addAction(
                        noteFolder.getName());
                copyAction->setData(noteFolder.getLocalPath());
                copyAction->setToolTip(noteFolder.getLocalPath());
                copyAction->setStatusTip(noteFolder.getLocalPath());
            }
    }

    QList<Tag> tagList = Tag::fetchAll();

    // show the tagging menu if at least one tag is present
    if (tagList.count() > 0) {
        tagMenu = noteMenu.addMenu(tr("&Tag selected notes with..."));

        Q_FOREACH(Tag tag, tagList) {
                QAction *action = tagMenu->addAction(
                        tag.getName());
                action->setData(tag.getId());
                action->setToolTip(tag.getName());
                action->setStatusTip(tag.getName());
            }
    }

    QStringList noteNameList;
    Q_FOREACH(QListWidgetItem *item, ui->notesListWidget->selectedItems()) {
            QString name = item->text();
            Note note = Note::fetchByName(name);
            if (note.isFetched()) {
                noteNameList << note.getName();
            }
        }

    QList<Tag> tagRemoveList = Tag::fetchAllWithLinkToNoteNames(
            noteNameList);

    // show the remove tags menu if at least one tag is present
    if (tagRemoveList.count() > 0) {
        tagRemoveMenu = noteMenu.addMenu(
                tr("&Remove tag from selected notes..."));

        Q_FOREACH(Tag tag, tagRemoveList) {
                QAction *action = tagRemoveMenu->addAction(
                        tag.getName());
                action->setData(tag.getId());
                action->setToolTip(tag.getName());
                action->setStatusTip(tag.getName());
            }
    }

    QAction *removeAction = noteMenu.addAction(tr("&Remove notes"));
    noteMenu.addSeparator();
    QAction *selectAllAction = noteMenu.addAction(tr("Select &all notes"));

    QAction *selectedItem = noteMenu.exec(globalPos);
    if (selectedItem) {
        if (selectedItem->parent() == moveDestinationMenu) {
            // move notes
            QString destinationFolder = selectedItem->data().toString();
            moveSelectedNotesToFolder(destinationFolder);
        } else if (selectedItem->parent() == copyDestinationMenu) {
            // copy notes
            QString destinationFolder = selectedItem->data().toString();
            copySelectedNotesToFolder(destinationFolder);
        } else if (selectedItem->parent() == tagMenu) {
            // tag notes
            Tag tag = Tag::fetch(selectedItem->data().toInt());

            if (tag.isFetched()) {
                tagSelectedNotes(tag);
            }
        } else if (selectedItem->parent() == tagRemoveMenu) {
            // remove tag from notes
            Tag tag = Tag::fetch(selectedItem->data().toInt());

            if (tag.isFetched()) {
                removeTagFromSelectedNotes(tag);
            }
        } else if (selectedItem == removeAction) {
            // remove notes
            removeSelectedNotes();
        } else if (selectedItem == selectAllAction) {
            // select all notes
            selectAllNotes();
        }
    }
}

void MainWindow::on_actionSelect_all_notes_triggered() {
    selectAllNotes();
}

/**
 * @brief create the additional menu entries for the note text edit field
 * @param pos
 */
void MainWindow::on_noteTextEdit_customContextMenuRequested(const QPoint &pos) {
    QPoint globalPos = ui->noteTextEdit->mapToGlobal(pos);
    QMenu *menu = ui->noteTextEdit->createStandardContextMenu();

    menu->addSeparator();

    QString linkTextActionName =
            ui->noteTextEdit->textCursor().selectedText() != "" ?
                tr("&Link selected text") : tr("Insert &link");
    QAction *linkTextAction = menu->addAction(linkTextActionName);
    linkTextAction->setShortcut(QKeySequence("Ctrl+L"));

    QAction *selectedItem = menu->exec(globalPos);
    if (selectedItem) {
        if (selectedItem == linkTextAction) {
            // handle the linking of text with a note
            handleTextNoteLinking();
        }
    }
}

void MainWindow::on_actionInsert_Link_to_note_triggered() {
    // handle the linking of text with a note
    handleTextNoteLinking();
}

void MainWindow::on_action_DuplicateText_triggered() {
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    textEdit->duplicateText();
}

void MainWindow::on_action_Back_in_note_history_triggered() {
    if (this->noteHistory.back()) {
        ui->searchLineEdit->clear();
        setCurrentNoteFromHistoryItem(
                this->noteHistory.getCurrentHistoryItem());
    }
}

void MainWindow::on_action_Forward_in_note_history_triggered() {
    if (this->noteHistory.forward()) {
        ui->searchLineEdit->clear();
        setCurrentNoteFromHistoryItem(
                this->noteHistory.getCurrentHistoryItem());
    }
}

void MainWindow::on_action_Shortcuts_triggered() {
    QDesktopServices::openUrl(
            QUrl("http://www.qownnotes.org/shortcuts/QOwnNotes"));
}

void MainWindow::on_action_Knowledge_base_triggered() {
    QDesktopServices::openUrl(QUrl("http://www.qownnotes.org/Knowledge-base"));
}

/**
 * @brief Inserts the current date in ISO 8601 format
 */
void MainWindow::on_actionInsert_current_time_triggered() {
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    QTextCursor c = textEdit->textCursor();
    QDateTime dateTime = QDateTime::currentDateTime();

    // insert the current date in ISO 8601 format
    c.insertText(dateTime.toString(Qt::SystemLocaleShortDate));
}

void MainWindow::on_actionOpen_List_triggered() {
    openTodoDialog();
}

/**
 * @brief Exports the current note as PDF (markdown)
 */
void MainWindow::on_action_Export_note_as_PDF_markdown_triggered() {
    exportNoteAsPDF(ui->noteTextView);
}

/**
 * @brief Exports the current note as PDF (text)
 */
void MainWindow::on_action_Export_note_as_PDF_text_triggered() {
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    exportNoteAsPDF(textEdit);
}

/**
 * @brief Prints the current note (markdown)
 */
void MainWindow::on_action_Print_note_markdown_triggered() {
    printNote(ui->noteTextView);
}

/**
 * @brief Prints the current note (text)
 */
void MainWindow::on_action_Print_note_text_triggered() {
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    printNote(textEdit);
}

/**
 * @brief Inserts a chosen image at the current cursor position in the note text edit
 */
void MainWindow::on_actionInsert_image_triggered() {
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setDirectory(QDir::homePath());
    dialog.setNameFilter(tr("Image files (*.jpg *.png *.gif)"));
    dialog.setWindowTitle(tr("Select image to insert"));
    int ret = dialog.exec();

    if (ret == QDialog::Accepted) {
        QStringList fileNames = dialog.selectedFiles();
        if (fileNames.count() > 0) {
            QString fileName = fileNames.at(0);

            QFile file(fileName);

            // insert the image
            insertMedia(&file);
        }
    }
}

/**
 * Inserts a media file into a note
 */
bool MainWindow::insertMedia(QFile *file) {
    QString text = getInsertMediaMarkdown(file);
    if (!text.isEmpty()) {
        QMarkdownTextEdit* textEdit = activeNoteTextEdit();
        QTextCursor c = textEdit->textCursor();

        // if we try to insert media in the first line of the note (aka.
        // note name) move the cursor to the last line
        if (currentNoteLineNumber() == 1) {
            c.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
            textEdit->setTextCursor(c);
        }

        // insert the image link
        c.insertText(text);

        return true;
    }

    return false;
}

/**
 * Returns the markdown of the inserted media file into a note
 */
QString MainWindow::getInsertMediaMarkdown(QFile *file) {
    if (file->exists() && (file->size() > 0)) {
        QDir mediaDir(notesPath + QDir::separator() + "media");

        // created the media folder if it doesn't exist
        if (!mediaDir.exists()) {
            mediaDir.mkpath(mediaDir.path());
        }

        QFileInfo fileInfo(file->fileName());

        // find a random name for the new file
        QString newFileName =
                QString::number(qrand()) + "." + fileInfo.suffix();

        // copy the file the the media folder
        file->copy(mediaDir.path() + QDir::separator() + newFileName);

        // return the image link
        // we add a "\n" in the end so that hoedown recognizes multiple images
        return "![" + fileInfo.baseName() + "](file://media/" +
                newFileName + ")\n";
    }

    return "";
}

/**
 * Returns the cursor's line number in the current note
 */
int MainWindow::currentNoteLineNumber()
{
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    QTextCursor cursor = textEdit->textCursor();

    QTextDocument *doc = textEdit->document();
    QTextBlock blk = doc->findBlock(cursor.position());
    QTextBlock blk2 = doc->begin();

    int i = 1;
    while ( blk != blk2 ) {
        blk2 = blk2.next();
        i++;
    }

    return i;
}

/**
 * @brief Opens a browser with the changelog page
 */
void MainWindow::on_actionShow_changelog_triggered() {
    QDesktopServices::openUrl(
            QUrl("http://www.qownnotes.org/changelog/QOwnNotes"));
}

void MainWindow::on_action_Find_text_in_note_triggered() {
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    textEdit->searchWidget()->activate();
}

/**
 * Asks the user for a password and encrypts the note text with it
 */
void MainWindow::on_action_Encrypt_note_triggered()
{
    currentNote.refetch();

    // return if there the note text is already encrypted
    if (currentNote.hasEncryptedNoteText()) {
        return;
    }

    QString labelText =
            tr("Please enter your <strong>password</strong> to encrypt the note."
            "<br />Keep in mind that you have to <strong>remember</strong> "
            "your password to read the content of the note<br /> and that you "
           "can <strong>only</strong> do that <strong>in QOwnNotes</strong>!");
    PasswordDialog* dialog = new PasswordDialog(this, labelText, true);
    int dialogResult = dialog->exec();

    // if user pressed ok take the password
    if (dialogResult == QDialog::Accepted) {
        QString password = dialog->password();

        // if password wasn't empty encrypt the note
        if (!password.isEmpty()) {
            currentNote.setCryptoPassword(password);
            currentNote.store();
            QString noteText = currentNote.encryptNoteText();
            ui->noteTextEdit->setPlainText(noteText);
        }
    }
}

/**
 * Enables or disables the encrypt note buttons
 */
void MainWindow::updateEncryptNoteButtons()
{
    currentNote.refetch();
    bool hasEncryptedNoteText = currentNote.hasEncryptedNoteText();

    ui->action_Encrypt_note->setEnabled(!hasEncryptedNoteText);
    ui->actionEdit_encrypted_note->setEnabled(hasEncryptedNoteText);
    ui->actionDecrypt_note->setEnabled(hasEncryptedNoteText);
}

/**
 * Attempt to decrypt note text
 */
void MainWindow::on_actionDecrypt_note_triggered()
{
    currentNote.refetch();
    if (!currentNote.hasEncryptedNoteText()) {
        return;
    }

    if (QMessageBox::warning(
            this, tr("Decrypt note and store it as plain text"),
            tr("Your note will be decrypted and stored as plain text gain. Keep "
                    "in mind that the unencrypted note will possibly be synced "
                    "to your server and sensitive text may be exposed!<br />"
                    "Do you want to decrypt your note?"),
            tr("&Decrypt"), tr("&Cancel"), QString::null,
            0, 1) == 1) {
        return;
    }

    askForEncryptedNotePasswordIfNeeded();

    if (currentNote.canDecryptNoteText()) {
        ui->encryptedNoteTextEdit->hide();
        ui->noteTextEdit->setText(currentNote.getDecryptedNoteText());
        ui->noteTextEdit->show();
        ui->noteTextEdit->setFocus();
    }
}

/**
 * Lets the user edit an encrypted note text in a 2nd text edit
 */
void MainWindow::on_actionEdit_encrypted_note_triggered()
{
    currentNote.refetch();
    if (!currentNote.hasEncryptedNoteText()) {
        return;
    }

    askForEncryptedNotePasswordIfNeeded(
            tr("<br />You will be able to edit your encrypted note."));

    if (currentNote.canDecryptNoteText()) {
        const QSignalBlocker blocker(ui->encryptedNoteTextEdit);
        Q_UNUSED(blocker);

        ui->noteTextEdit->hide();
        ui->encryptedNoteTextEdit->setText(currentNote.getDecryptedNoteText());
        ui->encryptedNoteTextEdit->show();
        ui->encryptedNoteTextEdit->setFocus();
        _noteViewNeedsUpdate = true;
    }
}

/**
 * Puts the encrypted text back to the note text edit
 */
void MainWindow::on_encryptedNoteTextEdit_textChanged()
{
    currentNote.storeNewDecryptedText(ui->encryptedNoteTextEdit->toPlainText());
}

/**
 * Opens the current note in an external editor
 */
void MainWindow::on_action_Open_note_in_external_editor_triggered()
{
    QSettings settings;
    QString externalEditorPath =
            settings.value("externalEditorPath").toString();

    // use the default editor if no other editor was set
    if (externalEditorPath.isEmpty()) {
        QUrl url = currentNote.fullNoteFileUrl();
        qDebug() << __func__ << " - 'url': " << url;

        // open note file in default application for the type of file
        QDesktopServices::openUrl(url);
    } else {
        QString path = currentNote.fullNoteFilePath();

        qDebug() << __func__ << " - 'externalEditorPath': " <<
        externalEditorPath;
        qDebug() << __func__ << " - 'path': " << path;

        QProcess process;

        // open note file in external editor
#ifdef Q_OS_MAC
        process.startDetached(
            "open", QStringList() << externalEditorPath << "--args" << path);
#else
        process.startDetached(externalEditorPath, QStringList() << path);
#endif
    }
}

/**
 * Exports the current note as markdown file
 */
void MainWindow::on_action_Export_note_as_markdown_triggered()
{
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDirectory(QDir::homePath());
    dialog.setNameFilter(tr("Markdown files (*.md)"));
    dialog.setWindowTitle(tr("Export current note as Markdown file"));
    dialog.selectFile(currentNote.getName() + ".md");
    int ret = dialog.exec();

    if (ret == QDialog::Accepted) {
        QStringList fileNames = dialog.selectedFiles();
        if (fileNames.count() > 0) {
            QString fileName = fileNames.at(0);

            if (QFileInfo(fileName).suffix().isEmpty()) {
                fileName.append(".md");
            }

            QFile file(fileName);

            qDebug() << "exporting note file: " << fileName;

            if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                qCritical() << file.errorString();
                return;
            }
            QTextStream out(&file);
            out.setCodec("UTF-8");
            out << ui->noteTextEdit->toPlainText();
            file.flush();
            file.close();
        }
    }
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    MetricsService::instance()->sendVisitIfEnabled("dialog/" + objectName());
}

void MainWindow::on_actionGet_invloved_triggered()
{
    QDesktopServices::openUrl(
            QUrl("http://www.qownnotes.org/Knowledge-base/"
                         "How-can-I-get-involved-with-QOwnNotes"));
}

/**
 * Sets a note bookmark on bookmark slot 0..9
 */
void MainWindow::storeNoteBookmark(int slot) {
    // return if note text edit doesn't have the focus
    if (!ui->noteTextEdit->hasFocus()) {
        return;
    }

    QTextCursor c = ui->noteTextEdit->textCursor();
    NoteHistoryItem item = NoteHistoryItem(&currentNote, c.position());
    noteBookmarks[slot] = item;

    showStatusBarMessage(
            tr("bookmarked note position at slot %1").arg(
                    QString::number(slot)), 3000);
}

/**
 * Loads and jumps to a note bookmark from bookmark slot 0..9
 */
void MainWindow::gotoNoteBookmark(int slot) {
    NoteHistoryItem item = noteBookmarks[slot];

    // check if the note (still) exists
    if (item.getNote().exists()) {
        ui->noteTextEdit->setFocus();
        setCurrentNoteFromHistoryItem(item);

        showStatusBarMessage(
                tr("jumped to bookmark position at slot %1").arg(
                        QString::number(slot)), 3000);
    }
}

/**
 * Inserts a code block at the current cursor position
 */
void MainWindow::on_actionInset_code_block_triggered()
{
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    QTextCursor c = textEdit->textCursor();
    QString selectedText = textEdit->textCursor().selectedText();

    if (selectedText.isEmpty()) {
        c.insertText("``");
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor);
        textEdit->setTextCursor(c);
    } else {
        // if the selected text has multiple lines add some new lines
        // on top and at the bottom of the selected text
        if (textEdit->textCursor().selection().toPlainText().contains("\n")) {
            selectedText = "\n" + selectedText + "\n";
        }

        c.insertText("`" + selectedText + "`");
    }
}

void MainWindow::on_actionNext_note_triggered()
{
    gotoNextNote();
}

/**
 * Jumps to the next visible note
 */
void MainWindow::gotoNextNote(int nextRow)
{
    if (firstVisibleNoteListRow == -1) {
        return;
    }

    // if no next row was set get one after the current row
    if (nextRow == -1) {
        nextRow = ui->notesListWidget->currentRow() + 1;
    }

    // if the row doesn't exist start with 0
    if (nextRow >= ui->notesListWidget->count()) {
        return gotoNextNote(0);
    }

    QListWidgetItem * item = ui->notesListWidget->item(nextRow);

    // if item is hidden try the next row
    if (item->isHidden()) {
        return gotoNextNote(nextRow + 1);
    }

    ui->notesListWidget->setCurrentRow(nextRow);
}

void MainWindow::on_actionPrevious_Note_triggered()
{
    gotoPreviousNote();
}

/**
 * Jumps to the previous visible note
 */
void MainWindow::gotoPreviousNote(int previousRow)
{
    if (firstVisibleNoteListRow == -1) {
        return;
    }

    // if no previous row was set get one before the current row
    if (previousRow == -1) {
        previousRow = ui->notesListWidget->currentRow() -1;
    }

    // if the row is below 0 use the last row
    if (previousRow < 0) {
        return gotoPreviousNote(ui->notesListWidget->count() - 1);
    }

    QListWidgetItem * item = ui->notesListWidget->item(previousRow);

    // if the item is hidden try the previous
    if (item->isHidden()) {
        previousRow--;

        // if the row is below 0 use the last row
        if (previousRow < 0) {
            previousRow = ui->notesListWidget->count() - 1;
        }

        return gotoPreviousNote(previousRow);
    }

    ui->notesListWidget->setCurrentRow(previousRow);
}

void MainWindow::on_actionToggle_distraction_free_mode_triggered()
{
    toggleDistractionFreeMode();
}

/**
 * Tracks an action
 */
void MainWindow::trackAction(QAction *action) {
    MetricsService::instance()->sendVisitIfEnabled(
            "action/" + action->objectName());
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    ui->noteTextEdit->setPaperMargins(event->size().width());
    ui->encryptedNoteTextEdit->setPaperMargins(event->size().width());
}

/**
 * Toggles the visibility of the toolbars
 */
void MainWindow::on_actionShow_toolbar_triggered(bool checked)
{
    ui->mainToolBar->setVisible(checked);
    _formattingToolbar->setVisible(checked);
    _insertingToolbar->setVisible(checked);
    _encryptionToolbar->setVisible(checked);
    _windowToolbar->setVisible(checked);
}

/**
 * Toggles the checked state of the "show toolbar" checkbox in the main menu
 */
void MainWindow::mainToolbarVisibilityChanged(bool visible)
{
    const QSignalBlocker blocker(ui->actionShow_toolbar);
    {
        Q_UNUSED(blocker);
        ui->actionShow_toolbar->setChecked(visible);
    }
}

void MainWindow::dfmEditorWidthActionTriggered(QAction *action) {
    QSettings settings;
    settings.setValue("DistractionFreeMode/editorWidthMode",
                      action->whatsThis().toInt());

    ui->noteTextEdit->setPaperMargins(this->width());
    ui->encryptedNoteTextEdit->setPaperMargins(this->width());
}

/**
 * Allows files to be dropped to QOwnNotes
 */
void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
    }
}

/**
 * Handles the copying of notes to the current notes folder
 */
void MainWindow::dropEvent(QDropEvent *e) {
    handleInsertingFromMimeData(e->mimeData());
}

/**
 * Handles the inserting of media files and notes from a mime data, for example
 * produced by a drop event or a paste action
 */
void MainWindow::handleInsertingFromMimeData(const QMimeData *mimeData) {
    if (mimeData->hasHtml()) {
        insertHtml(mimeData->html());
    } else if (mimeData->hasUrls()) {
        int successCount = 0;
        int failureCount = 0;
        int skipCount = 0;

        foreach(const QUrl &url, mimeData->urls()) {
                QString path(url.toLocalFile());
                QFileInfo fileInfo(path);
                qDebug() << __func__ << " - 'path': " << path;

                if (fileInfo.isReadable()) {
                    QFile *file = new QFile(path);

                    // only allow markdown and text files to be copied as note
                    if (isValidNoteFile(file)) {
                        // copy file to notes path
                        bool success = file->copy(
                                notesPath + QDir::separator() +
                                fileInfo.fileName());

                        if (success) {
                            successCount++;
                        } else {
                            failureCount++;
                        }
                    // only allow image files to be inserted as image
                    } else if (isValidMediaFile(file)) {
                        showStatusBarMessage(tr("inserting image"));

                        // insert the image
                        insertMedia(file);

                        showStatusBarMessage(tr("done inserting image"), 3000);
                    } else {
                        skipCount++;
                    }
                } else {
                    skipCount++;
                }
            }

        QString message;
        if (successCount > 0) {
            message += tr("copied %n note(s) to %1", "", successCount)
                    .arg(notesPath);
        }

        if (failureCount > 0) {
            if (!message.isEmpty()) {
                message += ", ";
            }

            message += tr(
                    "failed to copy %n note(s) (most likely already existing)",
                    "", failureCount);
        }

        if (skipCount > 0) {
            if (!message.isEmpty()) {
                message += ", ";
            }

            message += tr(
                    "skipped copying of %n note(s) "
                            "(no markdown or text file or not readable)",
                    "", skipCount);
        }

        if (!message.isEmpty()) {
            showStatusBarMessage(message, 5000);
        }
    } else if (mimeData->hasImage()) {
        // get the image from mime data
        QImage image = mimeData->imageData().value<QImage>();

        if (!image.isNull()) {
            showStatusBarMessage(tr("saving temporary image"));

            QTemporaryFile tempFile(
                    QDir::tempPath() + QDir::separator() +
                    "qownnotes-media-XXXXXX.png");

            if (tempFile.open()) {
                // save temporary png image
                image.save(tempFile.fileName(), "PNG");

                // insert media into note
                QFile *file = new QFile(tempFile.fileName());

                showStatusBarMessage(tr("inserting image"));
                insertMedia(file);

                showStatusBarMessage(tr("done inserting image"), 3000);
            } else {
                showStatusBarMessage(
                        tr("temporary file can't be opened"), 3000);
            }
        }
    }
}

/**
 * Inserts html as markdown in the current note
 * Images are also downloaded
 */
void MainWindow::insertHtml(QString html) {
    qDebug() << __func__ << " - 'html': " << html;

    // remove some blocks
    html.remove(QRegularExpression(
            "<head[^>]*>([^<]+)<\\/head>",
            QRegularExpression::CaseInsensitiveOption));

    html.remove(QRegularExpression(
            "<script[^>]*>([^<]+)<\\/script>",
            QRegularExpression::CaseInsensitiveOption));

    html.remove(QRegularExpression(
            "<style[^>]*>([^<]+)<\\/style>",
            QRegularExpression::CaseInsensitiveOption));

    // replace some html tags with markdown
    html.replace(QRegularExpression(
            "<strong[^>]*>([^<]+)<\\/strong>",
            QRegularExpression::CaseInsensitiveOption), "**\\1**");
    html.replace(QRegularExpression(
            "<b[^>]*>([^<]+)<\\/b>",
            QRegularExpression::CaseInsensitiveOption), "**\\1**");
    html.replace(QRegularExpression(
            "<em[^>]*>([^<]+)<\\/em>",
            QRegularExpression::CaseInsensitiveOption), "*\\1*");
    html.replace(QRegularExpression(
            "<i[^>]*>([^<]+)<\\/i>",
            QRegularExpression::CaseInsensitiveOption), "*\\1*");
    html.replace(QRegularExpression(
            "<h1[^>]*>([^<]+)<\\/h1>",
            QRegularExpression::CaseInsensitiveOption), "\n# \\1\n");
    html.replace(QRegularExpression(
            "<h2[^>]*>([^<]+)<\\/h2>",
            QRegularExpression::CaseInsensitiveOption), "\n## \\1\n");
    html.replace(QRegularExpression(
            "<h3[^>]*>([^<]+)<\\/h3>",
            QRegularExpression::CaseInsensitiveOption), "\n### \\1\n");
    html.replace(QRegularExpression("<h4[^>]*>([^<]+)<\\/h4>",
            QRegularExpression::CaseInsensitiveOption), "\n#### \\1\n");
    html.replace(QRegularExpression(
            "<h5[^>]*>([^<]+)<\\/h5>",
            QRegularExpression::CaseInsensitiveOption), "\n##### \\1\n");
    html.replace(QRegularExpression(
            "<br[^>]*>",
            QRegularExpression::CaseInsensitiveOption), "\n");
    html.replace(QRegularExpression(
            "<a[^>]+href=\"([^\"]+)\"[^>]*>([^<]+)<\\/a>",
            QRegularExpression::CaseInsensitiveOption), "[\\2](\\1)");

    // match image tags
    QRegularExpression re("<img[^>]+src=\"([^\"]+)\"[^>]*>",
                          QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator i = re.globalMatch(html);

    // find, download locally and replace all images
    while (i.hasNext()) {
        QRegularExpressionMatch match = i.next();
        QString imageTag = match.captured(0);
        QUrl imageUrl = QUrl(match.captured(1) );

        qDebug() << __func__ << " - 'imageUrl': " << imageUrl;

        if (!imageUrl.isValid()) {
            continue;
        }

        showStatusBarMessage(tr("downloading %1").arg(imageUrl.toString()));

        // try to get the suffix from the url
        QString suffix =
                imageUrl.toString().split(".", QString::SkipEmptyParts).last();
        if (suffix.isEmpty()) {
            suffix = "image";
        }

        // remove strings like "?b=16068071000" from the suffix
        suffix.remove(QRegularExpression("\\?.+$"));

        QTemporaryFile *tempFile = new QTemporaryFile(
                QDir::tempPath() + QDir::separator() + "media-XXXXXX." +
                        suffix);

        if (tempFile->open()) {
            // download the image to the temporary file
            if (downloadUrlToFile(imageUrl, tempFile)) {
                // copy image to media folder and generate markdown code for
                // the image
                QString markdownCode = getInsertMediaMarkdown(tempFile);
                if (!markdownCode.isEmpty()) {
                    // replace image tag with markdown code
                    html.replace(imageTag, markdownCode);
                }
            }
        }
    }

    showStatusBarMessage(tr("done downloading images"));

    // remove all html tags
    html.remove(QRegularExpression("<[^>]*>"));

    // remove the last character, that is broken
    html = html.left(html.size() - 1);

    qDebug() << __func__ << " - 'html': " << html;

    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    QTextCursor c = textEdit->textCursor();

    c.insertText(html);
}

/**
 * Evaluates if file is a note file
 */
bool MainWindow::isValidMediaFile(QFile *file) {
    QStringList mediaExtensions = QStringList() << "jpg" << "png" << "gif";
    QFileInfo fileInfo(file->fileName());
    QString extension = fileInfo.suffix();
    return mediaExtensions.contains(extension, Qt::CaseInsensitive);
}

/**
 * Evaluates if file is a media file
 */
bool MainWindow::isValidNoteFile(QFile *file) {
    QStringList mediaExtensions = QStringList() << "txt" << "md";
    QFileInfo fileInfo(file->fileName());
    QString extension = fileInfo.suffix();
    return mediaExtensions.contains(extension, Qt::CaseInsensitive);
}

void MainWindow::on_actionPaste_image_triggered()
{
    pasteMediaIntoNote();
}

/**
 * Handles the pasting of media into notes
 */
void MainWindow::pasteMediaIntoNote() {
    QClipboard *clipboard = QApplication::clipboard();
    const QMimeData * mimeData = clipboard->mimeData(QClipboard::Clipboard);
    handleInsertingFromMimeData(mimeData);
}

void MainWindow::on_actionShow_note_in_file_manager_triggered()
{
    Utils::Misc::openFolderSelect(currentNote.fullNoteFilePath());
}

/**
 * Inserts a bold block at the current cursor position
 */
void MainWindow::on_actionFormat_text_bold_triggered()
{
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    QTextCursor c = textEdit->textCursor();
    QString selectedText = textEdit->textCursor().selectedText();

    if (selectedText.isEmpty()) {
        c.insertText("****");
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
        textEdit->setTextCursor(c);
    } else {
        c.insertText("**" + selectedText + "**");
    }
}

/**
 * Inserts an italic block at the current cursor position
 */
void MainWindow::on_actionFormat_text_italic_triggered()
{
    QMarkdownTextEdit* textEdit = activeNoteTextEdit();
    QTextCursor c = textEdit->textCursor();
    QString selectedText = textEdit->textCursor().selectedText();

    if (selectedText.isEmpty()) {
        c.insertText("**");
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor);
        textEdit->setTextCursor(c);
    } else {
        c.insertText("*" + selectedText + "*");
    }
}

/**
 * Increases the note text font size by one
 */
void MainWindow::on_action_Increase_note_text_size_triggered()
{
    int fontSize = ui->noteTextEdit
            ->modifyFontSize(QOwnNotesMarkdownTextEdit::Increase);
    ui->encryptedNoteTextEdit->setStyles();
    ui->encryptedNoteTextEdit->highlighter()->parse();
    showStatusBarMessage(
            tr("Increased font size to %1 pt").arg(fontSize), 2000);
}

/**
 * Decreases the note text font size by one
 */
void MainWindow::on_action_Decrease_note_text_size_triggered()
{
    int fontSize = ui->noteTextEdit
            ->modifyFontSize(QOwnNotesMarkdownTextEdit::Decrease);
    ui->encryptedNoteTextEdit->setStyles();
    ui->encryptedNoteTextEdit->highlighter()->parse();
    showStatusBarMessage(
            tr("Decreased font size to %1 pt").arg(fontSize), 2000);
}

/**
 * Resets the note text font size
 */
void MainWindow::on_action_Reset_note_text_size_triggered()
{
    int fontSize = ui->noteTextEdit
            ->modifyFontSize(QOwnNotesMarkdownTextEdit::Reset);
    ui->encryptedNoteTextEdit->setStyles();
    ui->encryptedNoteTextEdit->highlighter()->parse();
    showStatusBarMessage(tr("Reset font size to %1 pt").arg(fontSize), 2000);
}

/**
 * Sets the note folder from the recent note folder combobox
 */
void MainWindow::on_noteFolderComboBox_currentIndexChanged(int index)
{
    int noteFolderId = ui->noteFolderComboBox->itemData(index).toInt();
    NoteFolder noteFolder = NoteFolder::fetch(noteFolderId);
    if (noteFolder.isFetched()) {
        changeNoteFolder(noteFolderId);
    }
}

/**
 * Reloads the tag list
 */
void MainWindow::reloadTagList()
{
    qDebug() << __func__ << " - 'reloadTagList'";

    int activeTagId = Tag::activeTagId();
    ui->tagListWidget->clear();

    // add an item to view all notes
    QListWidgetItem *allItem = new QListWidgetItem(
            tr("All notes (%1)").arg(QString::number(Note::countAll())));
    allItem->setToolTip(tr("show all notes"));
    allItem->setData(Qt::UserRole, -1);
    allItem->setFlags(allItem->flags() & ~Qt::ItemIsSelectable);
    allItem->setIcon(QIcon::fromTheme(
            "edit-copy",
            QIcon(":icons/breeze-qownnotes/16x16/edit-copy.svg")));
    ui->tagListWidget->addItem(allItem);

    // add an empty item
    QListWidgetItem *emptyItem = new QListWidgetItem();
    emptyItem->setData(Qt::UserRole, 0);
    emptyItem->setFlags(allItem->flags() & ~Qt::ItemIsSelectable);
    ui->tagListWidget->addItem(emptyItem);

    // add all tags as item
    QList<Tag> tagList = Tag::fetchAll();
    Q_FOREACH(Tag tag, tagList) {
            QListWidgetItem *item = new QListWidgetItem();
            item->setData(Qt::UserRole, tag.getId());
            setTagListWidgetName(item);
            item->setIcon(QIcon::fromTheme(
                    "tag", QIcon(":icons/breeze-qownnotes/16x16/tag.svg")));
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            ui->tagListWidget->addItem(item);

            // set the active item
            if (activeTagId == tag.getId()) {
                const QSignalBlocker blocker(ui->tagListWidget);
                Q_UNUSED(blocker);

                ui->tagListWidget->setCurrentItem(item);

                // set a name without link count so we can edit the name
                item->setText(tag.getName());
            }
        }
}

/**
 * Sets the name (and the tooltip) of a tag list widget item
 */
void MainWindow::setTagListWidgetName(QListWidgetItem *item) {
    if (item == NULL) {
        return;
    }

    int tagId = item->data(Qt::UserRole).toInt();
    Tag tag = Tag::fetch(tagId);

    if (!tag.isFetched()) {
        return;
    }

    int linkCount = tag.countLinkedNoteFileNames();

    QString name = tag.getName();
    QString text = name;
    if (tagId != Tag::activeTagId()) {
        text += QString(" (%1)").arg(linkCount);
    }

    item->setText(text);
    item->setToolTip(tr("show all notes tagged with '%1' (%2)")
                             .arg(name, QString::number(linkCount)));
}

/**
 * Creates a new tag
 */
void MainWindow::on_tagLineEdit_returnPressed()
{
    QString name = ui->tagLineEdit->text();
    if (name.isEmpty()) {
        return;
    }

    const QSignalBlocker blocker(this->noteDirectoryWatcher);
    Q_UNUSED(blocker);

    Tag tag;
    tag.setName(name);
    tag.store();
    reloadTagList();
}

/**
 * Updates a tag
 */
void MainWindow::on_tagListWidget_itemChanged(QListWidgetItem *item)
{
    Tag tag = Tag::fetch(item->data(Qt::UserRole).toInt());
    if (tag.isFetched()) {
        QString name = item->text();
        if (!name.isEmpty()) {
            const QSignalBlocker blocker(this->noteDirectoryWatcher);
            Q_UNUSED(blocker);

            tag.setName(name);
            tag.store();
            reloadTagList();
        }
    }
}

/**
 * Filters tags
 */
void MainWindow::on_tagLineEdit_textChanged(const QString &arg1)
{
    // search tags if at least one character was entered
    if (arg1.count() >= 1) {
        QList<QListWidgetItem*> foundItems = ui->tagListWidget->
                findItems(arg1, Qt::MatchContains);

        for (int i = 0; i < this->ui->tagListWidget->count(); ++i) {
            QListWidgetItem *item =
                    this->ui->tagListWidget->item(i);
            int tagId = item->data(Qt::UserRole).toInt();
            item->setHidden(!foundItems.contains(item) && (tagId > 0));
        }
    } else {
        // show all items otherwise
        for (int i = 0; i < this->ui->tagListWidget->count(); ++i) {
            QListWidgetItem *item =
                    this->ui->tagListWidget->item(i);
            item->setHidden(false);
        }
    }
}

/**
 * Shows or hides everything for the note tags
 */
void MainWindow::setupTags() {
    bool tagsEnabled = isTagsEnabled();

    ui->tagFrame->setVisible(tagsEnabled);
    ui->noteTagFrame->setVisible(tagsEnabled);
    ui->newNoteTagLineEdit->setVisible(false);
    ui->newNoteTagButton->setVisible(true);

#ifdef Q_OS_MAC
    // try to compensate for the different button top margins in OS X
    ui->noteTagFrame->layout()->setContentsMargins(0, 0, 0, 0);
    ui->noteTagButtonFrame->layout()->setContentsMargins(0, 8, 0, 0);
#endif

    const QSignalBlocker blocker(ui->actionToggle_tag_pane);
    Q_UNUSED(blocker);
    ui->actionToggle_tag_pane->setChecked(tagsEnabled);

    if (tagsEnabled) {
        reloadTagList();
        reloadCurrentNoteTags();
    }

    // filter the notes again
    filterNotes(false);
}

/**
 * Shows or hides everything for the markdown view
 */
void MainWindow::setupMarkdownView() {
    bool markdownViewEnabled = isMarkdownViewEnabled();

    ui->noteViewFrame->setVisible(markdownViewEnabled);

    const QSignalBlocker blocker(ui->actionToggle_markdown_preview);
    Q_UNUSED(blocker);
    ui->actionToggle_markdown_preview->setChecked(markdownViewEnabled);
}

/**
 * Shows or hides everything for the note edit pane
 */
void MainWindow::setupNoteEditPane() {
    bool paneEnabled = isNoteEditPaneEnabled();

    ui->noteEditFrame->setVisible(paneEnabled);

    const QSignalBlocker blocker(ui->actionToggle_note_edit_pane);
    Q_UNUSED(blocker);
    ui->actionToggle_note_edit_pane->setChecked(paneEnabled);
}

/**
 * Toggles the note panes
 */
void MainWindow::on_actionToggle_tag_pane_toggled(bool arg1) {
    QSettings settings;
    settings.setValue("tagsEnabled", arg1);
    setupTags();
}

/**
 * Hides the note tag add button and shows the text edit
 */
void MainWindow::on_newNoteTagButton_clicked() {
    ui->newNoteTagLineEdit->setVisible(true);
    ui->newNoteTagLineEdit->setFocus();
    ui->newNoteTagLineEdit->selectAll();
    ui->newNoteTagButton->setVisible(false);
}

/**
 * Links a note to the tag entered after pressing return
 * in the note tag line edit
 */
void MainWindow::on_newNoteTagLineEdit_returnPressed() {
    QString text = ui->newNoteTagLineEdit->text();

    // create a new tag if it doesn't exist
    Tag tag = Tag::fetchByName(text);
    if (!tag.isFetched()) {
        const QSignalBlocker blocker(this->noteDirectoryWatcher);
        Q_UNUSED(blocker);

        tag.setName(text);
        tag.store();
        reloadTagList();
    }

    // link the current note to the tag
    if (tag.isFetched()) {
        const QSignalBlocker blocker(this->noteDirectoryWatcher);
        Q_UNUSED(blocker);

        tag.linkToNote(currentNote);
        reloadCurrentNoteTags();
    }
}

/**
 * Hides the note tag line edit after editing
 */
void MainWindow::on_newNoteTagLineEdit_editingFinished() {
    ui->newNoteTagLineEdit->setVisible(false);
    ui->newNoteTagButton->setVisible(true);
}

/**
 * Reloads the note tag buttons for the current note
 */
void MainWindow::reloadCurrentNoteTags() {
    // remove all remove-tag buttons
    QLayoutItem *child;
    while ((child = ui->noteTagButtonFrame->layout()->takeAt(0)) != 0) {
        delete child->widget();
        delete child;
    }

    // add all new remove-tag buttons
    QList<Tag> tagList = Tag::fetchAllOfNote(currentNote);
    Q_FOREACH(Tag tag, tagList) {
            QPushButton* button = new QPushButton(tag.getName(),
                                                  ui->noteTagButtonFrame);
            button->setIcon(QIcon::fromTheme(
                    "xml-attribute-delete",
                    QIcon(":icons/breeze-qownnotes/16x16/"
                                  "xml-attribute-delete.svg")));
            button->setToolTip(
                    tr("remove tag '%1' from note").arg(tag.getName()));
            button->setObjectName(
                    "removeNoteTag" + QString::number(tag.getId()));

            QObject::connect(button, SIGNAL(clicked()),
                             this, SLOT(removeNoteTagClicked()));

            ui->noteTagButtonFrame->layout()->addWidget(button);
        }
}

/**
 * Removes a note tag link
 */
void MainWindow::removeNoteTagClicked() {
    QString objectName = sender()->objectName();
    if (objectName.startsWith("removeNoteTag")) {
        int tagId = objectName.remove("removeNoteTag").toInt();
        Tag tag = Tag::fetch(tagId);
        if (!tag.isFetched()) {
            return;
        }

        const QSignalBlocker blocker(noteDirectoryWatcher);
        Q_UNUSED(blocker);

        tag.removeLinkToNote(currentNote);
        reloadCurrentNoteTags();
    }
}

/**
 * Allows the user to add a tag to the current note
 */
void MainWindow::on_action_new_tag_triggered() {
    if (!ui->actionToggle_tag_pane->isChecked()) {
        ui->actionToggle_tag_pane->setChecked(true);
    }

    on_newNoteTagButton_clicked();
}

/**
 * Sets a new active tag if an other tag was selected
 */
void MainWindow::on_tagListWidget_currentItemChanged(
        QListWidgetItem *current, QListWidgetItem *previous) {
    if (current == NULL) {
        return;
    }

    int tagId = current->data(Qt::UserRole).toInt();
    Tag tag = Tag::fetch(tagId);
    tag.setAsActive();

    if (tag.isFetched()) {
        const QSignalBlocker blocker(ui->searchLineEdit);
        Q_UNUSED(blocker);

        ui->searchLineEdit->clear();
    }

    filterNotes();

    const QSignalBlocker blocker2(ui->tagListWidget);
    Q_UNUSED(blocker2);

    // this is a workaround so we can have the note counts in the tag
    // name and edit it at the same time
    setTagListWidgetName(current);
    setTagListWidgetName(previous);
}

/**
 * Reloads the current note folder
 */
void MainWindow::on_action_Reload_note_folder_triggered() {
    buildNotesIndex();
    loadNoteDirectoryList();
    currentNote.refetch();
    setNoteTextFromNote(&currentNote);
}

void MainWindow::on_actionToggle_markdown_preview_toggled(bool arg1) {
    QSettings settings;
    settings.setValue("markdownViewEnabled", arg1);

    // setup the markdown view
    setupMarkdownView();

    // setup the main splitter again for the vertical note pane visibility
    setupMainSplitter();
}

void MainWindow::on_actionToggle_note_edit_pane_toggled(bool arg1) {
    QSettings settings;
    settings.setValue("noteEditPaneEnabled", arg1);

    // setup the note edit pane
    setupNoteEditPane();

    // setup the main splitter again for the vertical note pane visibility
    setupMainSplitter();
}

void MainWindow::on_actionUse_vertical_preview_layout_toggled(bool arg1) {
    QSettings settings;
    settings.setValue("verticalPreviewModeEnabled", arg1);

    // setup the main splitter again
    setupMainSplitter();
}
