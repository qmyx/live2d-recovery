#include "window.h"

#include "moc_reader.h"
#include "project_writer.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <exception>
#include <utility>

namespace {

QLabel* textLabel(const QString& text, const char* name, QWidget* parent = nullptr)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(name));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

QFrame* card(QWidget* parent = nullptr)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("card"));
    return frame;
}

QString currentException()
{
    try {
        throw;
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    } catch (const QString& error) {
        return error;
    } catch (...) {
        return QStringLiteral("发生未识别的错误，处理已停止。");
    }
}

struct ReadResult {
    std::shared_ptr<recovery::Model> model;
    QString error;
};

}

RecoveryWindow::RecoveryWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Live2D Recovery"));
    resize(1020, 760);
    setMinimumSize(820, 640);
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget#page { background: #F5F5F1; }
        QLabel { color: #26342B; background: transparent; }
        QLabel#title { font-size: 20px; font-weight: 600; }
        QLabel#subtitle, QLabel#hint, QLabel#statCaption { color: #6C776E; }
        QLabel#section { font-weight: 600; font-size: 14px; }
        QLabel#statValue { font-size: 23px; font-weight: 600; }
        QLabel#modelName { font-size: 14px; font-weight: 600; }
        QLabel#supportState { font-weight: 600; }
        QFrame#card { background: #FFFFFF; border: 1px solid #DFE4DC; border-radius: 10px; }
        QLineEdit { background: #FAFBF8; border: 1px solid #D9DFD5; border-radius: 6px;
                    color: #26342B; padding: 9px 11px; selection-background-color: #365C45; }
        QLineEdit:focus { border-color: #668770; }
        QLineEdit:disabled { color: #8B938B; background: #F1F3EF; }
        QPushButton { background: #FFFFFF; border: 1px solid #D4DCD1; border-radius: 6px;
                      color: #314537; padding: 9px 15px; }
        QPushButton:hover { background: #F0F4EE; border-color: #91A18D; }
        QPushButton:pressed { background: #E4EBE0; }
        QPushButton:disabled { color: #929B91; background: #F3F4F0; border-color: #E3E6DF; }
        QPushButton#restoreButton { background: #365C45; color: #FFFFFF;
                                   border-color: #365C45; font-weight: 600; padding: 12px 24px; }
        QPushButton#restoreButton:hover { background: #2D513B; }
        QPushButton#restoreButton:pressed { background: #23422F; }
        QPushButton#restoreButton:disabled { background: #DCE3D9; color: #879282;
                                            border-color: #DCE3D9; }
        QPlainTextEdit { border: 0; background: transparent; color: #536156;
                         selection-background-color: #365C45; }
        QProgressBar { background: #E6EBE1; border: 0; border-radius: 2px; max-height: 4px; }
        QProgressBar::chunk { background: #527459; border-radius: 2px; }
        QScrollArea { border: 0; background: transparent; }
        QToolTip { color: #26342B; background: #FFFFFF; border: 1px solid #D9DFD5; padding: 5px; }
    )"));

    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("page"));
    setCentralWidget(page);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(26, 22, 26, 22);
    layout->setSpacing(18);

    auto* heading = new QHBoxLayout;
    auto* icon = new QLabel;
    icon->setPixmap(QApplication::windowIcon().pixmap(44, 44));
    icon->setFixedSize(44, 44);
    heading->addWidget(icon);
    auto* headingText = new QVBoxLayout;
    headingText->setSpacing(3);
    headingText->addWidget(textLabel(QStringLiteral("Live2D Recovery"), "title"));
    headingText->addWidget(textLabel(QStringLiteral("模型工程恢复"), "subtitle"));
    heading->addSpacing(8);
    heading->addLayout(headingText);
    heading->addStretch();
    layout->addLayout(heading);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(16);

    auto* workspace = new QHBoxLayout;
    workspace->setSpacing(16);
    auto* files = card();
    auto* fileLayout = new QVBoxLayout(files);
    fileLayout->setContentsMargins(22, 20, 22, 20);
    fileLayout->setSpacing(12);
    fileLayout->addWidget(textLabel(QStringLiteral("模型文件"), "section"));
    fileLayout->addWidget(textLabel(QStringLiteral("选择 .model3.json，并保留同目录的 MOC3 和贴图。"), "hint"));
    auto* inputRow = new QHBoxLayout;
    input_ = new QLineEdit;
    input_->setObjectName(QStringLiteral("inputPath"));
    input_->setPlaceholderText(QStringLiteral("尚未选择模型"));
    input_->setReadOnly(true);
    input_->setMinimumWidth(120);
    inputButton_ = new QPushButton(QStringLiteral("选择文件"));
    inputRow->addWidget(input_, 1);
    inputRow->addWidget(inputButton_);
    fileLayout->addLayout(inputRow);
    fileLayout->addSpacing(6);

    modelName_ = textLabel(QStringLiteral("等待读取"), "modelName");
    modelName_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fileLayout->addWidget(modelName_);
    auto* statistics = new QGridLayout;
    statistics->setHorizontalSpacing(20);
    statistics->setVerticalSpacing(3);
    const QString captions[] = {QStringLiteral("网格"), QStringLiteral("参数"),
                                QStringLiteral("变形器"), QStringLiteral("贴图")};
    for (int i = 0; i < 4; ++i) {
        counts_[i] = textLabel(QStringLiteral("—"), "statValue");
        statistics->addWidget(counts_[i], 0, i);
        statistics->addWidget(textLabel(captions[i], "statCaption"), 1, i);
        statistics->setColumnStretch(i, 1);
    }
    fileLayout->addLayout(statistics);
    fileLayout->addSpacing(12);
    fileLayout->addWidget(textLabel(QStringLiteral("保存位置"), "section"));
    auto* outputRow = new QHBoxLayout;
    output_ = new QLineEdit;
    output_->setObjectName(QStringLiteral("outputDirectory"));
    output_->setPlaceholderText(QStringLiteral("选择结果保存位置"));
    output_->setMinimumWidth(120);
    outputButton_ = new QPushButton(QStringLiteral("选择目录"));
    outputRow->addWidget(output_, 1);
    outputRow->addWidget(outputButton_);
    fileLayout->addLayout(outputRow);
    fileLayout->addWidget(textLabel(QStringLiteral("保存为新的 .cmo3 文件；原始模型文件不会被修改。"), "hint"));
    fileLayout->addStretch();
    workspace->addWidget(files, 2);

    auto* support = card();
    support->setMinimumWidth(235);
    support->setMaximumWidth(310);
    auto* supportLayout = new QVBoxLayout(support);
    supportLayout->setContentsMargins(20, 20, 20, 18);
    supportLayout->setSpacing(12);
    supportLayout->addWidget(textLabel(QStringLiteral("恢复范围"), "section"));
    supportState_ = textLabel(QStringLiteral("读取后检查支持情况"), "supportState");
    supportLayout->addWidget(supportState_);
    features_ = new QPlainTextEdit;
    features_->setObjectName(QStringLiteral("featureList"));
    features_->setReadOnly(true);
    features_->setPlainText(QStringLiteral("工具将检查实际读取到的模型结构。遇到尚未实现的结构时，会列出原因并停止导出。\n\nMOC3 不包含原始 PSD 图层和编辑历史，无法完整还原这些信息。"));
    features_->setMinimumHeight(170);
    supportLayout->addWidget(features_, 1);
    workspace->addWidget(support, 1);
    contentLayout->addLayout(workspace);

    auto* logCard = card();
    auto* logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(20, 16, 20, 12);
    logLayout->setSpacing(8);
    logLayout->addWidget(textLabel(QStringLiteral("运行记录"), "section"));
    log_ = new QPlainTextEdit;
    log_->setObjectName(QStringLiteral("log"));
    log_->setReadOnly(true);
    log_->setPlaceholderText(QStringLiteral("读取与保存的结果会显示在这里。关闭窗口后不会保留记录。"));
    log_->setMinimumHeight(90);
    log_->setMaximumHeight(120);
    log_->setMaximumBlockCount(400);
    logLayout->addWidget(log_);
    contentLayout->addWidget(logCard);
    contentLayout->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    progress_ = new QProgressBar;
    progress_->setObjectName(QStringLiteral("progress"));
    progress_->setRange(0, 1);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(4);
    layout->addWidget(progress_);
    auto* footer = new QHBoxLayout;
    footer->setSpacing(12);
    status_ = textLabel(QStringLiteral("请选择模型文件。"), "status");
    status_->setMinimumWidth(160);
    footer->addWidget(status_, 1);
    folderButton_ = new QPushButton(QStringLiteral("打开结果目录"));
    folderButton_->setVisible(false);
    footer->addWidget(folderButton_);
    restoreButton_ = new QPushButton(QStringLiteral("恢复工程"));
    restoreButton_->setObjectName(QStringLiteral("restoreButton"));
    restoreButton_->setMinimumWidth(140);
    restoreButton_->setEnabled(false);
    footer->addWidget(restoreButton_);
    layout->addLayout(footer);

    connect(inputButton_, &QPushButton::clicked, this, &RecoveryWindow::chooseInput);
    connect(outputButton_, &QPushButton::clicked, this, &RecoveryWindow::chooseOutput);
    connect(output_, &QLineEdit::textChanged, this, &RecoveryWindow::updateActions);
    connect(restoreButton_, &QPushButton::clicked, this, &RecoveryWindow::restore);
    connect(folderButton_, &QPushButton::clicked, this, [this] {
        if (!savedPath_.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(savedPath_).absolutePath()));
        }
    });
}

void RecoveryWindow::chooseInput()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择模型文件"), QString(),
        QStringLiteral("Live2D 模型描述 (*.model3.json)"));
    if (!path.isEmpty()) {
        inspectInput(path);
    }
}

void RecoveryWindow::chooseOutput()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择结果保存位置"), output_->text());
    if (!path.isEmpty()) {
        output_->setText(QDir::toNativeSeparators(path));
    }
}

void RecoveryWindow::inspectInput(const QString& path)
{
    if (busy_) {
        return;
    }
    model_.reset();
    savedPath_.clear();
    folderButton_->hide();
    input_->setText(QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()));
    input_->setToolTip(input_->text());
    modelName_->setText(QStringLiteral("正在读取…"));
    for (auto* count : counts_) {
        count->setText(QStringLiteral("—"));
    }
    supportState_->setText(QStringLiteral("正在检查模型结构"));
    features_->clear();
    setBusy(true, QStringLiteral("正在读取模型…"));
    appendLog(QStringLiteral("开始读取模型文件。"));

    auto result = std::make_shared<ReadResult>();
    auto* worker = QThread::create([result, path] {
        try {
            result->model = std::make_shared<recovery::Model>(recovery::readModel(path));
        } catch (...) {
            result->error = currentException();
        }
    });
    worker->setParent(this);
    connect(worker, &QThread::finished, this, [this, result] {
        setBusy(false, QString());
        if (!result->error.isEmpty() || !result->model) {
            showFailure(result->error.isEmpty() ? QStringLiteral("未能读取模型数据。") : result->error);
            modelName_->setText(QStringLiteral("未读取到模型"));
            supportState_->setText(QStringLiteral("无法检查"));
            return;
        }
        model_ = result->model;
        showModel();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void RecoveryWindow::showModel()
{
    modelName_->setText(model_->displayName.isEmpty()
                            ? QFileInfo(model_->inputFile).fileName()
                            : model_->displayName);
    counts_[0]->setText(QString::number(model_->meshes.size()));
    counts_[1]->setText(QString::number(model_->parameters.size()));
    counts_[2]->setText(QString::number(model_->deformers.size()));
    counts_[3]->setText(QString::number(model_->textures.size()));

    QStringList notes;
    if (!model_->unsupported.isEmpty()) {
        supportState_->setText(QStringLiteral("暂不支持这个模型"));
        notes << QStringLiteral("以下结构尚不支持，恢复已禁用：");
        for (const auto& item : model_->unsupported) {
            notes << QStringLiteral("• ") + item;
        }
        status_->setText(QStringLiteral("模型包含尚未支持的结构，请查看恢复范围。"));
        appendLog(QStringLiteral("读取完成，但有 %1 项结构尚不支持，未创建工程。")
                      .arg(model_->unsupported.size()));
    } else {
        supportState_->setText(QStringLiteral("结构检查通过"));
        notes << QStringLiteral("已读取的模型结构可以写入恢复工程。请在 Cubism Editor 中检查生成结果。")
              << QStringLiteral("原始 PSD 图层和编辑历史无法恢复。");
        status_->setText(output_->text().trimmed().isEmpty()
                             ? QStringLiteral("读取完成，请选择保存位置。")
                             : QStringLiteral("读取完成，可以恢复工程。"));
        appendLog(QStringLiteral("读取完成：%1 个网格，%2 个参数，%3 个变形器。")
                      .arg(model_->meshes.size()).arg(model_->parameters.size())
                      .arg(model_->deformers.size()));
    }
    if (!model_->observations.isEmpty()) {
        notes << QString() << QStringLiteral("读取说明：");
        for (const auto& item : model_->observations) {
            notes << QStringLiteral("• ") + item;
        }
    }
    features_->setPlainText(notes.join(QLatin1Char('\n')));
    updateActions();
}

QString RecoveryWindow::availableOutputPath() const
{
    const QDir directory(output_->text().trimmed());
    QString path = directory.absoluteFilePath(QStringLiteral("recovered.cmo3"));
    int number = 2;
    while (QFileInfo::exists(path)) {
        path = directory.absoluteFilePath(QStringLiteral("recovered (%1).cmo3").arg(number++));
    }
    return path;
}

void RecoveryWindow::restore()
{
    if (busy_ || !model_ || !model_->unsupported.isEmpty()) {
        return;
    }
    const QFileInfo directory(output_->text().trimmed());
    if (!directory.exists() || !directory.isDir()) {
        showFailure(QStringLiteral("保存目录不存在，请选择一个现有目录。"));
        return;
    }

    const QString path = availableOutputPath();
    savedPath_.clear();
    folderButton_->hide();
    setBusy(true, QStringLiteral("正在写入工程…"));
    appendLog(QStringLiteral("开始写入新工程：%1").arg(QFileInfo(path).fileName()));
    auto error = std::make_shared<QString>();
    const auto model = model_;
    auto* worker = QThread::create([model, path, error] {
        try {
            if (QFileInfo::exists(path)) {
                throw QStringLiteral("目标文件已经存在，已停止保存以防覆盖。");
            }
            recovery::writeProject(*model, path);
        } catch (...) {
            *error = currentException();
        }
    });
    worker->setParent(this);
    connect(worker, &QThread::finished, this, [this, path, error] {
        setBusy(false, QString());
        if (!error->isEmpty()) {
            showFailure(*error);
            return;
        }
        const QFileInfo result(path);
        if (!result.isFile() || result.size() == 0) {
            showFailure(QStringLiteral("写入没有产生有效文件，请查看运行记录。"));
            return;
        }
        savedPath_ = path;
        folderButton_->show();
        progress_->setValue(1);
        status_->setText(QStringLiteral("工程已保存，请在 Cubism Editor 中检查。"));
        appendLog(QStringLiteral("保存完成：%1").arg(QDir::toNativeSeparators(path)));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void RecoveryWindow::setBusy(bool busy, const QString& message)
{
    busy_ = busy;
    inputButton_->setEnabled(!busy);
    outputButton_->setEnabled(!busy);
    output_->setEnabled(!busy);
    folderButton_->setEnabled(!busy);
    progress_->setRange(0, busy ? 0 : 1);
    progress_->setValue(0);
    if (!message.isEmpty()) {
        status_->setText(message);
    }
    updateActions();
}

void RecoveryWindow::updateActions()
{
    restoreButton_->setEnabled(!busy_ && model_ && model_->unsupported.isEmpty()
                               && !output_->text().trimmed().isEmpty());
    output_->setToolTip(output_->text());
}

void RecoveryWindow::showFailure(const QString& message)
{
    status_->setText(QStringLiteral("处理未完成，请查看运行记录。"));
    appendLog(QStringLiteral("错误：") + message);
    updateActions();
}

void RecoveryWindow::appendLog(const QString& message)
{
    log_->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))
                         + QStringLiteral("  ") + message);
}

void RecoveryWindow::closeEvent(QCloseEvent* event)
{
    if (busy_) {
        status_->setText(QStringLiteral("处理仍在进行，请稍候再关闭窗口。"));
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}
