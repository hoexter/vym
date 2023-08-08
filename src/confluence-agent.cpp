#include "confluence-agent.h"

#include <QMessageBox>
#include <QSslSocket>

#include <iostream> // FIXME-2 for debugging...

#include "branchitem.h"
#include "confluence-user.h"
#include "file.h"
#include "mainwindow.h"
#include "misc.h"
#include "vymmodel.h"
#include "warningdialog.h"

extern Main *mainWindow;
extern QDir vymBaseDir;
extern QString confluencePassword;
extern Settings settings;
extern bool debug;

bool ConfluenceAgent::available() 
{ 
    if (!QSslSocket::supportsSsl())
        return false;
    if ( settings.value("/atlassian/confluence/username", "").toString().isEmpty())
        return false;

    if ( settings.value("/atlassian/confluence/url", "").toString().isEmpty())
        return false;

    return true;
}

ConfluenceAgent::ConfluenceAgent() { 
    //qDebug() << "Constr. ConfluenceAgent";
    init(); 
}

ConfluenceAgent::ConfluenceAgent(BranchItem *bi)
{
    //qDebug() << "Constr. ConfluenceAgent selbi = " << bi;

    if (!bi) {
        qWarning("Const ConfluenceAgent: bi == nullptr");
        // This will leave the agent hanging around undeleted...
        return;
    }

    init();

    setBranch(bi);
}

ConfluenceAgent::~ConfluenceAgent()
{
    //qDebug() << "Destr ConfluenceAgent.";
    if (killTimer)
        delete killTimer;
}

void ConfluenceAgent::init()
{
    jobType = Undefined;
    jobStep = -1;
    abortJob = false;

    killTimer = nullptr;

    networkManager = new QNetworkAccessManager(this);

    modelID = 0;    // invalid ID

    killTimer = new QTimer(this);
    killTimer->setInterval(15000);
    killTimer->setSingleShot(true);

    QObject::connect(killTimer, SIGNAL(timeout()), this, SLOT(timeout()));

    apiURL = baseURL + "/rest/api";
    baseURL = settings.value("/atlassian/confluence/url", "baseURL").toString();
    
    // Settings
    exportImage = false;

    // Read credentials 
    authUsingPAT = 
        settings.value("/atlassian/confluence/authUsingPAT", true).toBool();
    if (authUsingPAT)
        personalAccessToken =
            settings.value("/atlassian/confluence/PAT", "undefined").toString();
    else {
        username =
            settings.value("/atlassian/confluence/username", "user_johnDoe").toString();
        if (!confluencePassword.isEmpty())
            password = confluencePassword;
        else
            password = 
                settings.value("/atlassian/confluence/password", "").toString();
    }

    if (!authUsingPAT && password.isEmpty()) {
        // Set global password
        if (!mainWindow->settingsConfluence()) 
            abortJob = true;
    }
}

void ConfluenceAgent::setJobType(JobType jt)
{
    jobType = jt;
}

void ConfluenceAgent::setBranch(BranchItem *bi)
{
    if (!bi) {
        qWarning() << "ConfluenceAgent::setBranch  bi == nullptr";
        abortJob = true;
    } else {
        branchID = bi->getID();
        VymModel *model = bi->getModel();
        modelID = model->getModelID();
    }
}

void ConfluenceAgent::setModelID(uint id)
{
    modelID = id;
}

void ConfluenceAgent::setPageURL(const QString &u)
{
    pageURL = u;
}

void ConfluenceAgent::setNewPageName(const QString &t)
{
    newPageName = t;
}

void ConfluenceAgent::setUploadFilePath(const QString &fp)
{
    uploadFilePath = fp;
}

void ConfluenceAgent::startJob()
{
    if (jobStep > 0) {
        unknownStepWarning();
        finishJob();
    } else {
        jobStep = 0;
        continueJob();
    }
}

void ConfluenceAgent::continueJob()
{
    if (abortJob) {
        finishJob();
        return;
    }

    jobStep++;

    VymModel *model;

    qDebug() << "CA::contJob " << jobType << " Step: " << jobStep;

    switch(jobType) {
        case CopyPagenameToHeading:
            switch(jobStep) {
                case 1:
                    startGetPageSourceRequest(pageURL);
                    return;
                case 2:
                    startGetPageDetailsRequest();
                    return;
                case 3:
                    model = mainWindow->getModel(modelID);
                    if (model) {
                        BranchItem *bi = (BranchItem *)(model->findID(branchID));

                        if (bi) {
                            QString h = spaceKey + ": " + pageObj["title"].toString();
                            model->setHeading(h, bi);
                        }
                        else
                            qWarning() << "CA::continueJob couldn't find branch "
                                       << branchID;
                    }
                    else
                        qWarning() << "CA::continueJob couldn't find model " << modelID;
                    break;  // FIXME-0 and now?
                default:
                    unknownStepWarning();
                    break;  // FIXME-0 and now?
            };
            break;  // FIXME-0 and now?

        case NewPage:   // FIXME-0 add upload image
            switch(jobStep) {
                case 1:
                    if (pageURL.isEmpty()) {
                        qWarning() << "CA::contJob NewPage: pageURL is empty";
                        finishJob();
                        return;
                    }
                    if (newPageName.isEmpty()) {
                        qWarning() << "CA::contJob NewPage: newPageName is empty";
                        finishJob();
                        return;
                    }

                    mainWindow->statusMessage(
                        QString("Starting to create Confluence page %1").arg(pageURL));

                    // Check if parent page with url already exists and get pageID, spaceKey
                    startGetPageSourceRequest(pageURL);
                    return;
                case 2:
                    // Create new page with parent url
                    startCreatePageRequest();
                    return;
                case 3:
                    //qDebug() << "CA::finished  Created page with ID: " << pageObj["id"].toString();
                    mainWindow->statusMessage(
                        QString("Created Confluence page %1").arg(pageURL));
                    finishJob();
                    return;
                default:
                    unknownStepWarning();
            };

            break;  // FIXME-0 and now?

        case UpdatePage:
            switch(jobStep) {
                case 1:
                    if (pageURL.isEmpty()) {
                        qWarning() << "CA::contJob UpdatePage: pageURL is empty";
                        finishJob();
                        return;
                    }

                    mainWindow->statusMessage(
                        QString("Starting to update Confluence page %1").arg(pageURL));

                    // Check if page with url already exists and get pageID, spaceKey
                    startGetPageSourceRequest(pageURL);
                    return;
                case 2:
                    // Get title, which is required by Confluence to update a page
                    startGetPageDetailsRequest();
                    return;
                case 3:
                    // Create attachment with image of map, if required
                    qDebug() << "exportImage= " << exportImage;
                    if (exportImage) {
                        startCreateAttachmentRequest();
                        return;
                    }
                    // Go on to update page
                    jobStep = jobStep + 2;
                case 4:
                    if (exportImage) {
                        startUpdateAttachmentRequest();
                        jobStep++;
                        return;
                    }
                    // Attachment with image of map is already there, update it
                case 5:
                    // Update page with parent url
                    if (newPageName.isEmpty())
                            newPageName = pageObj["title"].toString();
                    startUpdatePageRequest();
                    return;
                case 6:
                    //qDebug() << "CA::finished  Updated page with ID: " << pageObj["id"].toString();
                    mainWindow->statusMessage(
                        QString("Updated Confluence page %1").arg(pageURL));
                    finishJob();
                    return;
                default:
                    unknownStepWarning();
            };

            break;  // FIXME-0 and now?
        case UserInfo:
            switch(jobStep) {
                case 1:
                    // qDebug() << "CA:: begin getting UserInfo";
                    startGetUserInfoRequest();
                    return;
                case 2: {
                        QJsonArray array = pageObj["results"].toArray();
                        QJsonObject userObj;
                        QJsonObject u;
                        ConfluenceUser user;
                        userList.clear();
                        for (int i = 0; i < array.size(); ++i) {
                            userObj = array[i].toObject();

                            u = userObj["user"].toObject();
                            user.setTitle( userObj["title"].toString());
                            user.setURL( "https://" + baseURL + "/"
                                    + "display/~" + u["username"].toString());
                            user.setUserKey( u["userKey"].toString());
                            user.setUserName( u["username"].toString());
                            user.setDisplayName( u["displayName"].toString());
                            userList << user;
                        }
                        emit (foundUsers(userList));
                        finishJob();
                    }
                    break;  // FIXME-0 and now?
                default:
                    unknownStepWarning();
            }
            break;  // FIXME-0 and now?
        default:
            qWarning() << "ConfluenceAgent::continueJob   unknown jobType " << jobType;
    }
}

void ConfluenceAgent::finishJob()
{
    deleteLater();
}

void ConfluenceAgent::unknownStepWarning()
{
    qWarning() << "CA::contJob  unknow step in jobType = " 
        << jobType 
        << "jobStep = " << jobStep;
}

void ConfluenceAgent::getUsers(const QString &usrQuery)
{
    userQuery = usrQuery;
    if (usrQuery.contains(QRegExp("\\W+"))) {
        qWarning() << "ConfluenceAgent::getUsers  Forbidden characters in " << usrQuery;
        return;
    }

    setJobType(UserInfo);
    startJob();
}

void ConfluenceAgent::startGetPageSourceRequest(QUrl requestedURL)
{
    //qDebug() << "CA::startGetPageSourceRequest " << requestedURL;
    if (!requestedURL.toString().startsWith("http"))
        requestedURL.setPath("https://" + requestedURL.path());

    QUrl url = requestedURL;

   // FIXME-0 Refactor everything to use createRequest method
    QNetworkRequest request = QNetworkRequest(url);

    // Basic authentication in header
    QString headerData;
    if (authUsingPAT)
        headerData = QString("Bearer %1").arg(personalAccessToken);
    else {
        QString concatenated = username + ":" + password;
        QByteArray data = concatenated.toLocal8Bit().toBase64();
        headerData = "Basic " + data;
    }
    request.setRawHeader("Authorization", headerData.toLocal8Bit());

    if (debug)
    {
        qDebug() << "CA::startGetPageSourceRequest: header = " + headerData;
        qDebug() << "CA::startGetPageSourceRequest: url = " + request.url().toString();
    }

    killTimer->start();

    connect(networkManager, &QNetworkAccessManager::finished,
        this, &ConfluenceAgent::pageSourceReceived);

    networkManager->get(request);
}

void ConfluenceAgent::startGetPageDetailsRequest()
{
    if (debug) qDebug() << "CA::startGetPageDetailsRequest" << pageID;

    // Authentication in URL  (only SSL!)
    QString query = "https://" 
        + baseURL + apiURL 
        + "/content/" + pageID + "?expand=metadata.labels,version";

    QNetworkRequest request = QNetworkRequest(QUrl(query));

    // Basic authentication in header
    QString headerData;
    if (authUsingPAT)
        headerData = QString("Bearer %1").arg(personalAccessToken);
    else {
        QString concatenated = username + ":" + password;
        QByteArray data = concatenated.toLocal8Bit().toBase64();
        headerData = "Basic " + data;
    }
    request.setRawHeader("Authorization", headerData.toLocal8Bit());

    connect(networkManager, &QNetworkAccessManager::finished,
        this, &ConfluenceAgent::pageDetailsReceived);

    killTimer->start();

    networkManager->get(request);
}

void ConfluenceAgent::startCreatePageRequest()
{
    // qDebug() << "CA::startCreatePageRequest";

    QString url = "https://" + baseURL + apiURL + "/content";

    QNetworkRequest request = QNetworkRequest(QUrl(url));

    // Basic authentication in header
    QString headerData;
    if (authUsingPAT)
        headerData = QString("Bearer %1").arg(personalAccessToken);
    else {
        QString concatenated = username + ":" + password;
        QByteArray data = concatenated.toLocal8Bit().toBase64();
        headerData = "Basic " + data;
    }
    request.setRawHeader("Authorization", headerData.toLocal8Bit());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject payload;
    payload["type"] = "page";
    payload["title"] = newPageName;

    // Build array with ID of parent page
    QJsonObject ancestorsID;
    ancestorsID["id"] = pageID;
    QJsonArray ancestorsArray;
    ancestorsArray.append(ancestorsID);
    payload["ancestors"] = ancestorsArray;

    // Build object with space key
    QJsonObject skey;
    skey["key"] = spaceKey;
    payload["space"] = skey;

    // Build body
    QString body;
    if (!loadStringFromDisk(uploadFilePath, body))
    {
        qWarning() << "ConfluenceAgent: Couldn't read file to upload:" << uploadFilePath;
        finishJob();
        return;
    }

    QJsonObject innerStorageObj
    {
        {"value", body},
        {"representation", "storage"}
    };
    QJsonObject outerStorageObj;
    outerStorageObj["storage"] = innerStorageObj;
    payload["body"] = outerStorageObj;

    QJsonDocument doc(payload);
    QByteArray data = doc.toJson();

    connect(networkManager, &QNetworkAccessManager::finished,
        this, &ConfluenceAgent::pageUploaded);

    killTimer->start();

    networkManager->post(request, data);
}

QNetworkRequest ConfluenceAgent::createRequest(const QUrl &url)
{
    QNetworkRequest request = QNetworkRequest(url);

    QString headerData;
    if (authUsingPAT)
        headerData = QString("Bearer %1").arg(personalAccessToken);
    else {
        QString concatenated = username + ":" + password;
        QByteArray data = concatenated.toLocal8Bit().toBase64();
        headerData = "Basic " + data;
    }
    request.setRawHeader("Authorization", headerData.toLocal8Bit());

    return request;
}

void ConfluenceAgent::startUpdatePageRequest()
{
    if (debug) qDebug() << "CA::startUpdatePageRequest";

    QString url = "https://" + baseURL + apiURL + "/content" + "/" + pageID;

    QNetworkRequest request = createRequest(url);

    request.setHeader(
        QNetworkRequest::ContentTypeHeader,
        "application/json; charset=utf-8");

    QJsonObject payload;
    payload["id"] = pageID;
    payload["type"] = "page";
    payload["title"] = newPageName;

    // Build version object
    QJsonObject newVersionObj;
    QJsonObject oldVersionObj = pageObj["version"].toObject();

    newVersionObj["number"] = oldVersionObj["number"].toInt() + 1;
    payload["version"] = newVersionObj;

    // Build object with space key
    QJsonObject skey;
    skey["key"] = spaceKey;
    payload["space"] = skey;

    // Build body
    QString body;
    if (!loadStringFromDisk(uploadFilePath, body))
    {
        qWarning() << "ConfluenceAgent: Couldn't read file to upload:" << uploadFilePath;
        finishJob();
        return;
    }

    QJsonObject innerStorageObj
    {
        {"value", body},
        {"representation", "storage"}
    };
    QJsonObject outerStorageObj;
    outerStorageObj["storage"] = innerStorageObj;
    payload["body"] = outerStorageObj;

    QJsonDocument doc(payload);
    QByteArray data = doc.toJson();

    connect(networkManager, &QNetworkAccessManager::finished,
        this, &ConfluenceAgent::pageUploaded);

    killTimer->start();

    networkManager->put(request, data);
}

void ConfluenceAgent::startGetUserInfoRequest()
{
    if (debug) qDebug() << "CA::startGetInfoRequest for " << userQuery;

    QString query = "https://" + baseURL + apiURL
        + "/search?cql=user.fullname~" + userQuery;

    networkManager->disconnect();

    QNetworkRequest request = QNetworkRequest(QUrl(query));
//    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // Basic authentication in header
    QString headerData;
    if (authUsingPAT)
        headerData = QString("Bearer %1").arg(personalAccessToken);
    else {
        QString concatenated = username + ":" + password;
        QByteArray data = concatenated.toLocal8Bit().toBase64();
        headerData = "Basic " + data;
    }
    request.setRawHeader("Authorization", headerData.toLocal8Bit());

    connect(networkManager, &QNetworkAccessManager::finished,
        this, &ConfluenceAgent::userInfoReceived);

    killTimer->start();

    networkManager->get(request);
}

void ConfluenceAgent::startCreateAttachmentRequest()
{
    if (debug) qDebug() << "CA::startCreateAttachmentRequest";
    qDebug() << "*** startCreateAttachment begin";

    QString url = "https://" + baseURL + apiURL + "/content" + "/" + pageID + "/child/attachment";

    QNetworkRequest request = createRequest(url);
    request.setRawHeader("X-Atlassian-Token", "no-check");

    QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);   // FIXME-0 delete later

    attachmentName = "image.png";

    QHttpPart imagePart;
    imagePart.setHeader(
            QNetworkRequest::ContentDispositionHeader,

            // Name must be "file"
            QVariant(
                QString("form-data; name=\"file\"; filename=\"%1\"")
                    .arg(attachmentName)));
    imagePart.setHeader(
            QNetworkRequest::ContentTypeHeader,
            QVariant("image/jpeg"));

    QFile *file = new QFile("image.png");   // FIXME-0 delete later
    if (!file->open(QIODevice::ReadOnly))
        qWarning() << "Problem opening file!!!!!!!!!!!!!!!";    // FIXME-0 fix error handling
    imagePart.setBodyDevice(file);
    qDebug() << "        url=" << url;
    qDebug() << "  file size=" << file->size();
    multiPart->append(imagePart);
    file->setParent(multiPart); // delete later with the multiPart

    connect(networkManager, &QNetworkAccessManager::finished,
        this, &ConfluenceAgent::attachmentCreated);

    killTimer->start();

    QNetworkReply *reply = networkManager->post(request, multiPart);

    multiPart->setParent(reply);

    qDebug() << "*** startCreateAttachment end";
}

void ConfluenceAgent::startUpdateAttachmentRequest()
{
    if (debug) qDebug() << "CA::startUpdateAttachmentRequest";
    qDebug() << "*** startUpdateAttachment begin";

    QString url = "https://" + baseURL + apiURL + "/content" + "/" + pageID + "/child/attachment";

    QNetworkRequest request = createRequest(url);
    request.setRawHeader("X-Atlassian-Token", "no-check");

    QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);   // FIXME-0 delete later


    QHttpPart imagePart;
    imagePart.setHeader(
            QNetworkRequest::ContentDispositionHeader,

            // Name must be "file"
            QVariant("form-data; name=\"file\"; filename=\"image.png\""));
    imagePart.setHeader(
            QNetworkRequest::ContentTypeHeader,
            QVariant("image/jpeg"));

    QFile *file = new QFile("image.png");   // FIXME-0 delete later
    if (!file->open(QIODevice::ReadOnly))
        qWarning() << "Problem opening file!!!!!!!!!!!!!!!";    // FIXME-0 fix error handling
    imagePart.setBodyDevice(file);
    qDebug() << "        url=" << url;
    qDebug() << "  file size=" << file->size();
    multiPart->append(imagePart);
    file->setParent(multiPart); // we cannot delete the file now, so delete it with the multiPart

    connect(networkManager, &QNetworkAccessManager::finished,
        this, &ConfluenceAgent::attachmentUpdated);

    killTimer->start();

    QNetworkReply *reply = networkManager->post(request, multiPart);

    multiPart->setParent(reply);

    qDebug() << "*** startUpdateAttachment end";
}

bool ConfluenceAgent::wasRequestSuccessful(QNetworkReply *reply, const QString &requestDesc)
{
    if (reply->error()) {
        QByteArray readAll = reply->readAll();

        // Additionally print full error on console
        qWarning() << "         Step: " << requestDesc;
        qWarning() << "        Error: " << reply->error();
        qWarning() << "  Errorstring: " <<  reply->errorString();

        qDebug() << "    Request Url: " << reply->url() ;
        qDebug() << "      Operation: " << reply->operation() ;

        qDebug() << "      readAll: ";
        QJsonDocument jsdoc;
        jsdoc = QJsonDocument::fromJson(readAll);
        QString readAllFormatted = QString(jsdoc.toJson(QJsonDocument::Indented));
        cout << readAllFormatted.toStdString();

        /*
        qDebug() << "Request headers: ";
	QList<QByteArray> reqHeaders = reply->rawHeaderList();
	foreach( QByteArray reqName, reqHeaders )
        {
            QByteArray reqValue = reply->rawHeader( reqName );
            qDebug() << "  " << reqName << ": " << reqValue;
	}
        */

        if (reply->error() == QNetworkReply::AuthenticationRequiredError)
            QMessageBox::warning(
                nullptr, tr("Warning"),
                tr("Authentication problem when contacting Confluence") + "\n\n" + 
                requestDesc);
        else {
            QString msg = QString("QNetworkReply error when trying to \"%1\"\n\n").arg(requestDesc);
            WarningDialog warn;
            warn.setText(msg + "\n\n" + readAllFormatted);
            warn.showCancelButton(false);
            warn.exec();
        }

        finishJob();
        return false;
    } else
        return true;
}

void ConfluenceAgent::pageSourceReceived(QNetworkReply *reply)
{
    if (debug) qDebug() << "CA::pageSourceReceived";

    killTimer->stop();

    networkManager->disconnect();

    if (!wasRequestSuccessful(reply, "receive page source"))
        return;

    QString r = reply->readAll();

    // Find pageID
    QRegExp rx("\\sname=\"ajs-page-id\"\\scontent=\"(\\d*)\"");
    rx.setMinimal(true);

    if (rx.indexIn(r, 0) != -1) {
        pageID = rx.cap(1);
    }
    else {
        qWarning()
            << "ConfluenceAgent::pageSourceReveived Couldn't find page ID";
        //qWarning() << r;
        return;
    }

    // Find spaceKey 
    rx.setPattern("meta\\s*id=\"confluence-space-key\"\\s* "
                  "name=\"confluence-space-key\"\\s*content=\"(.*)\"");
    if (rx.indexIn(r, 0) != -1) {
        spaceKey = rx.cap(1);
    }
    else {
        qWarning() << "ConfluenceAgent::pageSourceReveived Couldn't find "
                      "space key in response";
        qWarning() << r;
        finishJob();
        return;
    }

    const QVariant redirectionTarget =
        reply->attribute(QNetworkRequest::RedirectionTargetAttribute);

    continueJob();
}

void ConfluenceAgent::pageDetailsReceived(QNetworkReply *reply)
{
    if (debug) qDebug() << "CA::pageDetailsReceived";

    killTimer->stop();

    networkManager->disconnect();

    if (!wasRequestSuccessful(reply, "receive page details"))
        return;

    QJsonDocument jsdoc;
    jsdoc = QJsonDocument::fromJson(reply->readAll());

    pageObj = jsdoc.object();
    // cout << jsdoc.toJson(QJsonDocument::Indented).toStdString();

    continueJob();
}

void ConfluenceAgent::pageUploaded(QNetworkReply *reply)
{
    if (debug) qDebug() << "CA::pageUploaded";

    killTimer->stop();

    networkManager->disconnect();

    if (!wasRequestSuccessful(reply, "upload page"))
        return;

    QJsonDocument jsdoc;
    jsdoc = QJsonDocument::fromJson(reply->readAll());
    pageObj = jsdoc.object();
    // cout << jsdoc.toJson(QJsonDocument::Indented).toStdString();
    continueJob();
}

void ConfluenceAgent::userInfoReceived(QNetworkReply *reply)
{
    if (debug) qDebug() << "CA::UserInfopageReceived";

    killTimer->stop();

    networkManager->disconnect();

    if (!wasRequestSuccessful(reply, "receive user info"))
        return;

    QString r = reply->readAll();

    QJsonDocument jsdoc;
    jsdoc = QJsonDocument::fromJson(r.toUtf8());
    pageObj = jsdoc.object();
    continueJob();
}

void ConfluenceAgent::attachmentCreated(QNetworkReply *reply)   // FIXME-0 when are reply, parts, and file deleted?
{
    if (debug) qDebug() << "CA::attachmentCreated";

    killTimer->stop();

    networkManager->disconnect();   // FIXME-0 what exactly happens at disconnect? why needed?

    if (reply->error() == QNetworkReply::ProtocolInvalidOperationError) {
        QByteArray readAll = reply->readAll();
        if (readAll.contains(
                    QString("Cannot add a new attachment with same file name as an existing attachment").toLatin1())) {
            // Replace existing attachment
            qDebug() << "AttachmentID unknown, stopping now"; 
            // FIXME-0 attachmentID = 
            finishJob();
            return;
        }
    }

    /* FIXME-0 remove
    if (!wasRequestSuccessful(reply, "create attachment"))
        return;
    */

    QString r = reply->readAll();
    qDebug() << "CA::attachmentCreated Successful. r=" << r;
    QList<QByteArray> reqHeaders = reply->request().rawHeaderList();
    foreach( QByteArray reqName, reqHeaders )
    {
        QByteArray reqValue = reply->request().rawHeader( reqName );
        qDebug() << "  " << reqName << ": " << reqValue;
    }

    QJsonDocument jsdoc;
    jsdoc = QJsonDocument::fromJson(r.toUtf8());
    attachmentObj = jsdoc.object();

    // Skip the step with updating attachment
    jobStep++;

    continueJob();
}

void ConfluenceAgent::attachmentUpdated(QNetworkReply *reply)
{
    if (debug) qDebug() << "CA::attachmentUpdated";

    killTimer->stop();

    networkManager->disconnect();

    if (reply->error()) {
        QByteArray readAll = reply->readAll();
        if (readAll.contains(QString("").toLatin1())) {
            // Replace existing attachment
        }
        
    }

    if (!wasRequestSuccessful(reply, "update attachment"))
        return;

    QString r = reply->readAll();
    qDebug() << "CA::attachmentUpdated Successful. r=" << r;
    QList<QByteArray> reqHeaders = reply->request().rawHeaderList();
    foreach( QByteArray reqName, reqHeaders )
    {
        QByteArray reqValue = reply->request().rawHeader( reqName );
        qDebug() << "  " << reqName << ": " << reqValue;
    }

    QJsonDocument jsdoc;
    jsdoc = QJsonDocument::fromJson(r.toUtf8());
    attachmentObj = jsdoc.object();
    continueJob();
}

void ConfluenceAgent::timeout()
{
    qWarning() << "ConfluenceAgent timeout!!   jobType = " << jobType;
}

#ifndef QT_NO_SSL
void ConfluenceAgent::sslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    QString errorString;
    foreach (const QSslError &error, errors) {
        if (!errorString.isEmpty())
            errorString += '\n';
        errorString += error.errorString();
    }

    reply->ignoreSslErrors();
    qWarning() << "ConfluenceAgent: One or more SSL errors has occurred: " << errorString;
    qWarning() << "Errors ignored.";
}
#endif
