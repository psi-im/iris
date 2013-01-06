#ifndef NETWORKACCESSMANAGER_H
#define NETWORKACCESSMANAGER_H

#include <QNetworkAccessManager>

class QNetworkDiskCache;
class QUrl;
class NetworkAccessManagerPrivate;
class ByteStream;

class NetworkSocketFactoryResult : public QObject
{
	Q_OBJECT

protected:
	bool _isFinished;
	ByteStream *_socket;

public:
	NetworkSocketFactoryResult(QObject *parent) :
		QObject(parent),
		_isFinished(false),
		_socket(NULL)
	{

	}

	inline bool isFinished() const { return _isFinished; }
	inline ByteStream *socket() const { return _socket; }
	inline void setSocket(ByteStream *socket = NULL)
	{
		// if socket is NULL, we were unable to retrieve one (kinda error)
		// socket may be in failed state too
		_socket = socket;
		_isFinished = true;
		emit socketConnected();
	}

signals:
	void socketConnected();
};




class NetworkSocketFactory
{
public:
	// by default creates internal result which works fine for http(s)://
	virtual NetworkSocketFactoryResult* socket(const QUrl &url);
};




class NetworkAccessManager : public QNetworkAccessManager
{
	Q_OBJECT
public:
	explicit NetworkAccessManager(QObject *parent = 0);
	~NetworkAccessManager();

	QNetworkDiskCache &cache() const;
	void setSocketFactory(NetworkSocketFactory *factory);

protected:
	QNetworkReply* createRequest(Operation op, const QNetworkRequest &req,
								 QIODevice *outgoingData = 0);
	
signals:
	
public slots:

private:
	NetworkAccessManagerPrivate *d;
};

#endif // NETWORKACCESSMANAGER_H
