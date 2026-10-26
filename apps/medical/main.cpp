#include "medical/MedicalWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("MedSignal"));
    app.setApplicationName(QStringLiteral("MedSignalWorkbench"));
    app.setApplicationVersion(QStringLiteral("1.1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Medical waveform replay and PhysioNet BIDMC respiratory baseline evaluation"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"dataset", "Original BIDMC CSV directory", "directory"});
    parser.process(app);
    saim::medical::MedicalWindow window;
    window.show();
    QString directory = parser.value("dataset");
    if (directory.isEmpty())
    {
        const QString packaged = QDir(app.applicationDirPath()).filePath(QStringLiteral("datasets/bidmc-demo"));
        directory = QDir(packaged).exists() ? packaged : QDir::current().filePath(QStringLiteral("datasets/bidmc"));
    }
    if (QDir(directory).exists()) { window.openDirectory(directory); }
    return app.exec();
}
