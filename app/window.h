#pragma once

#include <QMainWindow>
#include <memory>

#include "model.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QProgressBar;
class QCloseEvent;

class RecoveryWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit RecoveryWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void chooseInput();
    void chooseOutput();
    void inspectInput(const QString& path);
    void restore();
    void setBusy(bool busy, const QString& message);
    void updateActions();
    void showModel();
    void showFailure(const QString& message);
    void appendLog(const QString& message);
    QString availableOutputPath() const;

    std::shared_ptr<recovery::Model> model_;
    bool busy_ = false;
    QString savedPath_;

    QLineEdit* input_ = nullptr;
    QLineEdit* output_ = nullptr;
    QPushButton* inputButton_ = nullptr;
    QPushButton* outputButton_ = nullptr;
    QPushButton* restoreButton_ = nullptr;
    QPushButton* folderButton_ = nullptr;
    QLabel* modelName_ = nullptr;
    QLabel* counts_[4] = {};
    QLabel* supportState_ = nullptr;
    QPlainTextEdit* features_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
};
