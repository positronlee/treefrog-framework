/* Copyright (c) 2010-2019, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include <TWebApplication>
#include <TSystemGlobal>
#include <TAppSettings>
#include "tdatabasecontextmainthread.h"
#include "tcachefactory.h"
#include <QDir>
#include <QTextCodec>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdlib>
#include <thread>  // for hardware_concurrency()

constexpr auto DEFAULT_INTERNET_MEDIA_TYPE  = "text/plain";
constexpr auto DEFAULT_DATABASE_ENVIRONMENT = "product";


static QTextCodec *searchCodec(const char *name)
{
    QTextCodec *c = QTextCodec::codecForName(name);
    return (c) ? c : QTextCodec::codecForLocale();
}


/*!
  \class TWebApplication
  \brief The TWebApplication class provides an event loop for
  TreeFrog applications.
*/

/*!
  Constructor.
*/
TWebApplication::TWebApplication(int &argc, char **argv)
#ifdef TF_USE_GUI_MODULE
    : QApplication(argc, argv),
#else
    : QCoreApplication(argc, argv),
#endif
      _dbEnvironment(DEFAULT_DATABASE_ENVIRONMENT)
{
#if defined(Q_OS_WIN)
    installNativeEventFilter(new TNativeEventFilter);
#endif

    // parse command-line args
    _webRootAbsolutePath = ".";
    QStringList args = arguments();
    args.removeFirst();
    for (QStringListIterator i(args); i.hasNext(); ) {
        const QString &arg = i.next();
        if (arg.startsWith('-')) {
            if (arg == "-e" && i.hasNext()) {
                _dbEnvironment = i.next();
            }
            if (arg == "-i" && i.hasNext()) {
                _appServerId = i.next().toInt();
            }
        } else {
            if (QDir(arg).exists()) {
                _webRootAbsolutePath = arg;
                if (!_webRootAbsolutePath.endsWith(QDir::separator())) {
                    _webRootAbsolutePath += QDir::separator();
                }
            }
        }
    }

    QDir webRoot(_webRootAbsolutePath);
    if (webRoot.exists()) {
        _webRootAbsolutePath = webRoot.absolutePath() + QDir::separator();
    }

    // Sets application name
    QString appName = QDir(_webRootAbsolutePath).dirName();
    if (!appName.isEmpty()) {
        setApplicationName(appName);
    }

    // Creates settings objects
    TAppSettings::instantiate(appSettingsFilePath());
    QSettings loggerSetting(configPath() + "logger.ini", QSettings::IniFormat, this);
    QSettings validationSetting(configPath() + "validation.ini", QSettings::IniFormat, this);
    // Internet media types
    QSettings *mediaTypes;
    if (QFileInfo(configPath() + "internet_media_types.ini").exists()) {
        mediaTypes = new QSettings(configPath() + "internet_media_types.ini", QSettings::IniFormat, this);
    } else {
        mediaTypes = new QSettings(configPath() + "initializers" + QDir::separator() + "internet_media_types.ini", QSettings::IniFormat, this);
    }
    // Gets codecs
    _codecInternal = searchCodec(Tf::appSettings()->value(Tf::InternalEncoding).toByteArray().trimmed().data());
    _codecHttp = searchCodec(Tf::appSettings()->value(Tf::HttpOutputEncoding).toByteArray().trimmed().data());

    // Sets codecs for INI files
    loggerSetting.setIniCodec(_codecInternal);
    _loggerSetting = Tf::settingsToMap(loggerSetting);

    validationSetting.setIniCodec(_codecInternal);
    _validationSetting = Tf::settingsToMap(validationSetting);

    mediaTypes->setIniCodec(_codecInternal);
    _mediaTypes = Tf::settingsToMap(*mediaTypes);
    delete mediaTypes;

    // SQL DB settings
    QString dbsets = Tf::appSettings()->value(Tf::SqlDatabaseSettingsFiles).toString().trimmed();
    if (dbsets.isEmpty()) {
        dbsets = Tf::appSettings()->readValue("DatabaseSettingsFiles").toString().trimmed();
    }
    const QStringList files = dbsets.split(QLatin1Char(' '), QString::SkipEmptyParts);
    for (auto &f : files) {
        QSettings settings(configPath() + f, QSettings::IniFormat);
        settings.setIniCodec(_codecInternal);
        _sqlSettings.append(Tf::settingsToMap(settings));
    }

    // MongoDB settings
    QString mongoini = Tf::appSettings()->value(Tf::MongoDbSettingsFile).toString().trimmed();
    if (!mongoini.isEmpty()) {
        QString mnginipath = configPath() + mongoini;
        if (QFile(mnginipath).exists()) {
            QSettings settings(mnginipath, QSettings::IniFormat);
            settings.setIniCodec(_codecInternal);
            _mongoSetting = Tf::settingsToMap(settings);
        }
    }

    // Redis settings
    QString redisini = Tf::appSettings()->value(Tf::RedisSettingsFile).toString().trimmed();
    if (!redisini.isEmpty()) {
        QString redisinipath = configPath() + redisini;
        if (QFile(redisinipath).exists()) {
            QSettings settings(redisinipath, QSettings::IniFormat);
            settings.setIniCodec(_codecInternal);
            _redisSetting = Tf::settingsToMap(settings);
        }
    }
}


TWebApplication::~TWebApplication()
{ }

/*!
  Enters the main event loop and waits until exit() is called. Returns the
  value that was set to exit() (which is 0 if exit() is called via quit()).
*/
int TWebApplication::exec()
{
    resetSignalNumber();

#ifdef TF_USE_GUI_MODULE
    int ret = QApplication::exec();
#else
    int ret = QCoreApplication::exec();
#endif

    QEventLoop eventLoop;
    while (eventLoop.processEvents()) { }
    return ret;
}

/*!
  Returns true if the web root directory exists; otherwise returns false.
*/
bool TWebApplication::webRootExists() const
{
    return !_webRootAbsolutePath.isEmpty() && QDir(_webRootAbsolutePath).exists();
}

/*!
  Returns the absolute path of the public directory.
*/
QString TWebApplication::publicPath() const
{
    return webRootPath() + "public" + QDir::separator();
}

/*!
  Returns the absolute path of the config directory.
*/
QString TWebApplication::configPath() const
{
    return webRootPath() + "config" + QDir::separator();
}

/*!
  Returns the absolute path of the library directory.
*/
QString TWebApplication::libPath() const
{
    return webRootPath()+ "lib" + QDir::separator();
}

/*!
  Returns the absolute path of the log directory.
*/
QString TWebApplication::logPath() const
{
    return webRootPath() + "log" + QDir::separator();
}

/*!
  Returns the absolute path of the plugin directory.
*/
QString TWebApplication::pluginPath() const
{
    return webRootPath() + "plugin" + QDir::separator();
}

/*!
  Returns the absolute path of the tmp directory.
*/
QString TWebApplication::tmpPath() const
{
    return webRootPath() + "tmp" + QDir::separator();
}

/*!
  Returns true if the file of the application settings exists;
  otherwise returns false.
*/
bool TWebApplication::appSettingsFileExists() const
{
    return !Tf::appSettings()->appIniSettings->allKeys().isEmpty();
}

/*!
  Returns the absolute file path of the application settings.
*/
QString TWebApplication::appSettingsFilePath() const
{
    return configPath() + "application.ini";
}

/*!
  Returns a reference to the QSettings object for settings of the
  SQL database \a databaseId.
*/
const QVariantMap &TWebApplication::sqlDatabaseSettings(int databaseId) const
{
    static QVariantMap internalSettings = [&]() {
        // Settings of internal use databases
        QVariantMap settings;
        QString path = Tf::appSettings()->value(Tf::CacheSettingsFile).toString().trimmed();

        if (! path.isEmpty()) {
            // Copy settrings
            QSettings iniset(configPath() + path, QSettings::IniFormat);
            for (auto &k : iniset.allKeys()) {
                settings.insert(k, iniset.value(k));
            }
        }

        const QLatin1String singlefiledb("singlefiledb");
        QVariantMap defaultSettings = TCacheFactory::defaultSettings(singlefiledb);
        for (auto &k : defaultSettings.keys()) {
            auto val = settings.value(singlefiledb + "/" + k);
            auto defval = defaultSettings.value(k);
            if (val.toString().trimmed().isEmpty() && !defval.toString().trimmed().isEmpty()) {
                settings.insert(singlefiledb + "/" + k, defval);
            }
        }
        return settings;
    }();

    return (databaseId == databaseIdForInternalUse()) ? internalSettings : _sqlSettings[databaseId];
}

/*!
  Returns the number of SQL database settings files set by the setting
  \a DatabaseSettingsFiles in the application.ini.
*/
int TWebApplication::sqlDatabaseSettingsCount() const
{
    static int count = [&]() {
        int num = _sqlSettings.count();
        return (num > 0) ? num + 1 : num; // added 1 for internal use of DB
    }();
    return count;
}

/*!
  Returns true if SQL database is available; otherwise returns false.
*/
bool TWebApplication::isSqlDatabaseAvailable() const
{
    return sqlDatabaseSettingsCount() > 0;
}

/*!
 */
int TWebApplication::databaseIdForInternalUse() const
{
    static int id = [&]() {
        int cnt = sqlDatabaseSettingsCount();
        return (cnt > 0) ? cnt - 1 : 0;
    }();
    return id;
}

/*!
  Returns a reference to the QSettings object for settings of the
  MongoDB system.
*/
const QVariantMap &TWebApplication::mongoDbSettings() const
{
    return _mongoSetting;
}

/*!
  Returns true if MongoDB settings is available; otherwise returns false.
*/
bool TWebApplication::isMongoDbAvailable() const
{
    return !_mongoSetting.isEmpty();
}

/*!
  Returns a reference to the QSettings object for settings of the
  Redis system.
*/
const QVariantMap &TWebApplication::redisSettings() const
{
    return _redisSetting;
}

/*!
  Returns true if Redis settings is available; otherwise returns false.
*/
bool TWebApplication::isRedisAvailable() const
{
    return !_redisSetting.isEmpty();
}

/*!
  Returns the internet media type associated with the file extension
  \a ext.
*/
QByteArray TWebApplication::internetMediaType(const QString &ext, bool appendCharset)
{
    if (ext.isEmpty()) {
        return QByteArray();
    }

    QString type = _mediaTypes.value(ext.toLower(), DEFAULT_INTERNET_MEDIA_TYPE).toString();
    if (appendCharset && type.startsWith("text", Qt::CaseInsensitive)) {
        type += "; charset=" + Tf::app()->codecForHttpOutput()->name();
    }
    return type.toLatin1();
}

/*!
  Returns the error message for validation of the given \a rule. These messages
  are defined in the validation.ini.
*/
QString TWebApplication::validationErrorMessage(int rule) const
{
    return _validationSetting.value("ErrorMessage/" + QString::number(rule)).toString();
}

/*!
  Returns the module name for multi-processing that is set by the setting
  \a MultiProcessingModule in the application.ini.
*/
TWebApplication::MultiProcessingModule TWebApplication::multiProcessingModule() const
{
    if (_mpm == Invalid) {
        QString str = Tf::appSettings()->value(Tf::MultiProcessingModule).toString().toLower();
        if (str == "thread") {
            _mpm = Thread;
        } else if (str == "hybrid") {
#ifdef Q_OS_LINUX
            _mpm = Hybrid;
#else
            tSystemWarn("Unsupported MPM: hybrid  (Linux only)");
            tWarn("Unsupported MPM: hybrid  (Linux only)");
            _mpm = Thread;
#endif
        } else {
            tSystemWarn("Unsupported MPM: %s", qPrintable(str));
            tWarn("Unsupported MPM: %s", qPrintable(str));
            _mpm = Thread;
        }
    }
    return _mpm;
}

/*!
  Returns the maximum number of application servers, which is set in the
  application.ini.
*/
int TWebApplication::maxNumberOfAppServers() const
{
    static const int maxServers = ([]() -> int {
        QString mpmstr = Tf::appSettings()->value(Tf::MultiProcessingModule).toString().toLower();
        int num = Tf::appSettings()->readValue(QLatin1String("MPM.") + mpmstr + ".MaxAppServers").toInt();
        if (num <= 0) {
            num = qMax(std::thread::hardware_concurrency(), (uint)1);
            tSystemWarn("Sets max number of AP servers to %d", num);
        }
        return num;
    }());
    return maxServers;
}

/*!
  Maximum number of action threads allowed to start simultaneously
  per server process.
*/
int TWebApplication::maxNumberOfThreadsPerAppServer() const
{
    static int maxNumber = []() {
        int maxNum = 0;
        QString mpm = Tf::appSettings()->value(Tf::MultiProcessingModule).toString().toLower();

        switch (Tf::app()->multiProcessingModule()) {
        case TWebApplication::Thread:
            maxNum = Tf::appSettings()->readValue(QLatin1String("MPM.") + mpm + ".MaxThreadsPerAppServer").toInt();
            if (maxNum <= 0) {
                maxNum = Tf::appSettings()->readValue(QLatin1String("MPM.") + mpm + ".MaxServers", "128").toInt();
            }
            break;

        case TWebApplication::Hybrid:
            maxNum = Tf::appSettings()->readValue(QLatin1String("MPM.") + mpm + ".MaxWorkersPerAppServer").toInt();
            if (maxNum <= 0) {
                maxNum = Tf::appSettings()->readValue(QLatin1String("MPM.") + mpm + ".MaxWorkersPerServer", "128").toInt();
            }
            break;

        default:
            break;
        }
        return maxNum;
    }();

    return maxNumber;
}

/*!
  Returns the absolute file path of the routes config.
*/
QString TWebApplication::routesConfigFilePath() const
{
    return configPath() + "routes.cfg";
}

/*!
  Returns the absolute file path of the system log, which is set by the
  setting \a SystemLog.FilePath in the application.ini.
*/
QString TWebApplication::systemLogFilePath() const
{
    QFileInfo fi(Tf::appSettings()->value(Tf::SystemLogFilePath, "log/treefrog.log").toString());
    return (fi.isAbsolute()) ? fi.absoluteFilePath() : webRootPath() + fi.filePath();
}

/*!
  Returns the absolute file path of the access log, which is set by the
  setting \a AccessLog.FilePath in the application.ini.
*/
QString TWebApplication::accessLogFilePath() const
{
    QString name = Tf::appSettings()->value(Tf::AccessLogFilePath).toString().trimmed();
    if (name.isEmpty())
        return name;

    QFileInfo fi(name);
    return (fi.isAbsolute()) ? fi.absoluteFilePath() : webRootPath() + fi.filePath();
}

/*!
  Returns the absolute file path of the SQL query log, which is set by the
  setting \a SqlQueryLogFile in the application.ini.
*/
QString TWebApplication::sqlQueryLogFilePath() const
{
    QString path = Tf::appSettings()->value(Tf::SqlQueryLogFile).toString();
    if (!path.isEmpty()) {
        QFileInfo fi(path);
        path = (fi.isAbsolute()) ? fi.absoluteFilePath() : webRootPath() + fi.filePath();
    }
    return path;
}


void TWebApplication::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == _timer.timerId()) {
        if (signalNumber() >= 0) {
            tSystemDebug("TWebApplication trapped signal  number:%d", signalNumber());
            //timer.stop();   /* Don't stop this timer */
            QCoreApplication::exit(signalNumber());
        }
    } else {
#ifdef TF_USE_GUI_MODULE
        QApplication::timerEvent(event);
#else
        QCoreApplication::timerEvent(event);
#endif
    }
}


QThread *TWebApplication::databaseContextMainThread() const
{
    static TDatabaseContextMainThread *databaseThread = []() {
        auto *thread = new TDatabaseContextMainThread;
        thread->start();
        return thread;
    }();
    return databaseThread;
}


const QVariantMap &TWebApplication::getConfig(const QString &configName)
{
    auto cnf = configName.toLower();

    if (!_configMap.contains(cnf)) {
        QDir dir(configPath());
        QStringList filters = { configName + ".*", configName };
        const auto filist = dir.entryInfoList(filters);

        if (filist.isEmpty()) {
            tSystemWarn("No such config, %s", qPrintable(configName));
        } else {
            for (auto &fi : filist) {
                auto suffix = fi.completeSuffix().toLower();
                if (suffix == "ini") {
                    // INI format
                    QVariantMap map;
                    QSettings settings(fi.absoluteFilePath(), QSettings::IniFormat);
                    for (auto &k : (const QStringList &)settings.allKeys()) {
                        map.insert(k, settings.value(k));
                    }
                    _configMap.insert(cnf, map);
                    break;

                } else if (suffix == "json") {
                    // JSON format
                    QFile jsonFile(fi.absoluteFilePath());
                    if (jsonFile.open(QIODevice::ReadOnly)) {
                        auto json = QJsonDocument::fromJson(jsonFile.readAll()).object();
                        jsonFile.close();
                        _configMap.insert(cnf, json.toVariantMap());
                        break;
                    }

                } else {
                    tSystemWarn("Invalid format config, %s", qPrintable(fi.fileName()));
                }
            }
        }
    }
    return _configMap[cnf];
}


QVariant TWebApplication::getConfigValue(const QString &configName, const QString &key, const QVariant &defaultValue)
{
    return getConfig(configName).value(key, defaultValue);
}


QString TWebApplication::cacheBackend() const
{
    static QString backend = Tf::appSettings()->value(Tf::CacheBackend, "singlefiledb").toString().toLower();
    return backend;
}


/*!
  \fn QString TWebApplication::webRootPath() const
  Returns the absolute path of the web root directory.
*/

/*!
  \fn const QVariantMap &TWebApplication::appSettings() const
  Returns a reference to the QSettings object for settings of the
  web application, which file is the application.ini.
*/

/*!
  \fn const QVariantMap &TWebApplication::loggerSettings () const
  Returns a reference to the QSettings object for settings of the
  logger, which file is logger.ini.
*/

/*!
  \fn const QVariantMap &TWebApplication::validationSettings () const
  Returns a reference to the QSettings object for settings of the
  validation, which file is validation.ini.
*/

/*!
  \fn QTextCodec *TWebApplication::codecForInternal() const
  Returns a pointer to the codec used internally, which is set by the
  setting \a InternalEncoding in the application.ini. This codec is used
  by QObject::tr() and toLocal8Bit() functions.
*/

/*!
  \fn QTextCodec *TWebApplication::codecForHttpOutput() const
  Returns a pointer to the codec of the HTTP output stream used by an
  action view, which is set by the setting \a HttpOutputEncoding in
  the application.ini.
*/

/*!
  \fn QString TWebApplication::databaseEnvironment() const
  Returns the database environment, which string is used to load
  the settings in database.ini.
  \sa setDatabaseEnvironment(const QString &environment)
*/

/*!
  \fn void TWebApplication::watchConsoleSignal();
  Starts watching console signals i.e.\ registers a routine to handle the
  console signals.
*/

/*!
  \fn void TWebApplication::ignoreConsoleSignal();
  Ignores console signals, i.e.\ delivery of console signals will have no effect
  on the process.
*/

/*!
  \fn void TWebApplication::watchUnixSignal(int sig, bool watch);
  Starts watching the UNIX signal, i.e.\ registers a routine to handle the
  signal \a sig.
  \sa ignoreUnixSignal()
*/

/*!
  \fn void TWebApplication::ignoreUnixSignal(int sig, bool ignore)
  Ignores UNIX signals, i.e.\ delivery of the signal will have no effect on
  the process.
  \sa watchUnixSignal()
*/

/*!
  \fn void TWebApplication::timerEvent(QTimerEvent *)
  Reimplemented from QObject::timerEvent().
*/

/*!
  \fn int TWebApplication::signalNumber()
  Returns the integral number of the received signal.
*/
