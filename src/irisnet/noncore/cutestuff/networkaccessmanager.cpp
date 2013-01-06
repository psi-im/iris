#include <QNetworkRequest>
#include <QNetworkDiskCache>
#include <QBuffer>
#include <QRegExp>
#include <QNetworkReply>
#include <QtCrypto>

#include "networkaccessmanager.h"
#include "bytestream.h"
#include "bsocket.h"
#include "../xmpp/zlib/zlibdecompressor.h"

/*
  1) If layer has set out data layer than all processed data will be send to
  this out layer.
  for example. ssl layer has http as out layer. so ssl decoded data will be
  written to http layer (ssl layer calls writeIncoming of http layer).
  2) If out layer is not set then processed data written directly to read buffer
  and readyRead emitted.
  3) When out layer finish to process incoming data it will go to step 1

  Each layer connect readyRead signal of out layer to readyRead signal of itself
  in case it don't want to make some additional postprocessing. When some
  external entity starts reading by readyRead signal from top of layers stack,
  each layer checks if it has out layer and tries to read from this out layer.
  If there is no out layer (bottom of stack) it reads from own read buffer.
*/


class LayerStream : public ByteStream
{
	Q_OBJECT
public:
	LayerStream(QObject *parent) :
		ByteStream(parent),
		_dataOutLayer(NULL)
	{

	}

	/*
	  Sets source data layer for this layer.
	*/
	inline LayerStream* setDataOutLayer(LayerStream *dol)
	{
		return _dataOutLayer = dol;
		//connect(_dataOutLayer, SIGNAL(readyRead()), SIGNAL(readyRead()));
		//return dol;
	}

	/*bool isOpen() const
	{
		return dataSource != NULL;
	}*/

	// implemented here just in case. usually should be reimplemented or not used
	virtual void writeIncoming(const QByteArray &data)
	{
		handleOutData(data); // no processing in base class so move data out
	}

protected:
	void handleOutData(const QByteArray &data)
	{
		if (_dataOutLayer) {
			_dataOutLayer->writeIncoming(data);
		} else {
			appendRead(data);
			emit readyRead();
		}
	}

protected:
	LayerStream *_dataOutLayer;
};


//--------------------------------------------
// GzipStream
//--------------------------------------------
class GzipStream : public LayerStream
{
	Q_OBJECT

	ZLibDecompressor *zDec;
	QBuffer uncompressed;

public:
	GzipStream(QObject *parent) :
		LayerStream(parent)
	{
		uncompressed.setBuffer(&readBuf());
		zDec = new ZLibDecompressor(&uncompressed);
		connect(&uncompressed, SIGNAL(bytesWritten(qint64)), SLOT(decompressedWritten(qint64)));
	}

	void writeIncoming(const QByteArray &data)
	{
		zDec->write(data);
	}

private slots:
	void decompressedWritten(qint64 size)
	{
		if (size) {
			if (_dataOutLayer) {
				_dataOutLayer->writeIncoming(uncompressed.buffer());
				uncompressed.buffer().clear();
				uncompressed.seek(0);
			} else {
				emit readyRead();
			}
		}
	}
};



//--------------------------------------------
// ChunkedStream
//--------------------------------------------
class ChunkedStream : public LayerStream
{
	Q_OBJECT

	enum State
	{
		Header,
		Body,
		BodyEnd,
		Trailer
	};

	bool sizeParsed;
	State _state;
	quint64 chunkSize;
	quint64 chunkBytesLeft; // bytes left to read for current chunk
	static const quint8 tmpBufSize = 12;
	QByteArray tmpBuffer;

public:
	ChunkedStream(QObject *parent) :
		LayerStream(parent),
		sizeParsed(false),
		_state(Header)
	{
		tmpBuffer.reserve(tmpBufSize);
	}

	void writeIncoming(const QByteArray &data)
	{
		int index;
		QByteArray tail = QByteArray::fromRawData(data.constData(), data.size());
		while (tail.size()) {
			switch (_state) {
			case Header:
			{
				quint8 lastHeaderSize = (quint8)tmpBuffer.size();
				quint8 bufFree = tmpBufSize - lastHeaderSize;
				tmpBuffer += ((int)bufFree > tail.size() ? tail : QByteArray::fromRawData(
														   tail.constData(), bufFree));
				if ((index = tmpBuffer.indexOf("\r\n")) == -1) {
					if (!bufFree) {
						setError(ErrRead, "String for chunk header is too long");
					}
					return;
				}
				tmpBuffer.resize(index);
				int unparsedOffset = tmpBuffer.size() + 2 - lastHeaderSize;
				tail = QByteArray::fromRawData(tail.constData() + unparsedOffset,
											   tail.size() - unparsedOffset);


				chunkSize = tmpBuffer.toInt(&sizeParsed, 16);
				if (!sizeParsed) {
					setError(ErrRead, "chunk size parse failed");
					return;
				}
				chunkBytesLeft = chunkSize;
				tmpBuffer.clear(); // should be clean to make BodyEnd working
				_state = chunkSize? Body : Trailer; // 0 means the end of response
				break;
			}
			case Body:
			{
				QByteArray r = readTail(tail, chunkBytesLeft);
				chunkBytesLeft -= r.size();
				handleOutData(r);
				if (chunkBytesLeft) {
					break; // no enough data to finish chunk read
				}
				_state = BodyEnd;
			}
			case BodyEnd:
				tmpBuffer.append(readTail(tail, 2 - tmpBuffer.size()));
				if (tmpBuffer.size() == 2) {
					if (tmpBuffer[0] != '\r' || tmpBuffer[1] != '\n') {
						setError(ErrRead, "no \r\n at chunk end");
						return;
					}
					_state = Header;
				}
				break;
			case Trailer:
				// TODO
				break;
			}
		}
	}

private:
	QByteArray readTail(QByteArray &tail, int bytes) const
	{
		int rb = qMin<int>(bytes, tail.size());
		QByteArray ret = QByteArray::fromRawData(tail.constData(), rb);
		tail = QByteArray::fromRawData(tail.constData() + rb, tail.size() - rb);
		return ret;
	}
};




//--------------------------------------------
// HttpStream
//
// This layer assumes it receives raw data right from tcp or decoded ssl
// and on out it has some http unrelated data (html for example or contents
// of some file.). Layer internally creates another layers pipeline to handle
// http compression and chunked data (maybe other encodings in the future)
//--------------------------------------------
class HttpStream : public LayerStream
{
	Q_OBJECT

public:
	HttpStream(QObject *parent) :
		LayerStream(parent),
		headersReady(false)
	{

	}

	void writeIncoming(const QByteArray &data)
	{
		if (!data.size()) {
			return;
		}
		QByteArray realData;
		if (!headersReady) {
			int parsePos = qMax<int>(headersBuffer.size() - 3, 0);
			headersBuffer += data;
			if (headersBuffer.indexOf("\r\n\r\n", parsePos) == -1) {
				return;
			}
			if (parseHeaders(headersBuffer, parsePos)) {
				parsePos += 2;
				realData = QByteArray::fromRawData(headersBuffer.constData() + parsePos,
												   headersBuffer.size() - parsePos);
				headersReady = true;
				QByteArray header = headers.value("Content-Encoding").toLower();
				if (!header.isEmpty()) {
					QList<QByteArray> tes = header.split(',');
					while (tes.size()) {
						QByteArray lv = tes.takeLast().trimmed().toLower();
						if (lv == "gzip" || lv == "x-gzip" || lv == "deflate") {
							pipeLine.append(new GzipStream(this));
						}
					}
				}
				header = headers.value("Transfer-Encoding");
				if (!header.isEmpty()) {
					QList<QByteArray> tes = header.split(',');
					while (tes.size()) {
						QByteArray lv = tes.takeLast().trimmed().toLower();
						if (lv == "chunked") {
							pipeLine.append(new ChunkedStream(this));
						}
						if (lv == "gzip" || lv == "x-gzip" || lv == "deflate") {
							pipeLine.append(new GzipStream(this));
						}
					}
					headers.remove("Content-Length"); // by rfc2616 we have to ignore this header
				}
				if (pipeLine.count()) { // connect pipes
					for (int i = 0; i < pipeLine.count() - 1; i++) {
						pipeLine[i]->setDataOutLayer(pipeLine[i+1]);
					}
					connect(pipeLine.last(), SIGNAL(readyRead()), SLOT(pipeLine_readyReady()));
				}
				emit metaDataChanged();
			} else {
				qDebug("Invalid header: %s", headersBuffer.mid(
					parsePos, headersBuffer.indexOf("\r\n", parsePos)).data());
				setError(QNetworkReply::ProtocolFailure, "Invalid headers");
			}
		} else {
			realData = data;
		}
		if (realData.size()) {
			if (pipeLine.count()) {
				pipeLine[0]->writeIncoming(realData);
			} else {
				handleOutData(realData);
			}
		}
	}

private slots:
	void pipeLine_readyReady()
	{
		LayerStream *s = static_cast<LayerStream *>(sender());
		handleOutData(s->readAll());
	}

private:

	bool parseHeaders(const QByteArray &buffer, int &pos)
	{
		bool valid = true;
		bool statusRead = false;
		pos = 0;
		int endPos = 0;
		QByteArray lastKey;
		while ((endPos = buffer.indexOf("\r\n", pos)) != -1 && endPos != pos) {
			if (!statusRead) {
				QRegExp statusRE("^HTTP/(1.[01]) (\\d{3})( .*)?$");
				if (!statusRE.exactMatch(QString::fromLatin1(
									   buffer.constData() + pos, endPos - pos)))
				{
					valid = false;
					break;
				}
				httpVersion = statusRE.cap(1);
				statusCode = statusRE.cap(2).toInt();
				statusText = statusRE.cap(3).trimmed();
				statusRead = true;
			} else {
				QHash<QByteArray, QByteArray>::iterator it;
				if (buffer[pos] == ' ' || buffer[pos] == '\t') { // multiline value
					if (lastKey.isEmpty() || (it = headers.find(lastKey)) == headers.end()) {
						valid = false;
						break;
					}
					*it += ' ';
					*it += buffer.mid(pos, endPos - pos).trimmed();
				} else { // normal header line
					int sPos = buffer.indexOf(':', pos);
					if (sPos == -1 || sPos == pos || sPos > endPos) {
						valid = false;
						break;
					}
					QByteArray newKey = buffer.mid(pos, sPos - pos);
					QByteArray newValue = buffer.mid(sPos + 1, endPos - sPos - 1);
					if ((it = headers.find(newKey)) != headers.end()) { // by rfc we can combine so-named keys
						*it += ',';
						*it += newValue;
					} else {
						headers.insert(newKey, newValue);
						lastKey == newKey;
					}
				}
			}
			pos = endPos + 2;
		}
		return valid;
	}

signals:
	void metaDataChanged();

private:
	bool headersReady;
	quint16 statusCode;
	QString statusText;
	QString httpVersion;
	QByteArray headersBuffer;
	QList<LayerStream*> pipeLine;
	QHash<QByteArray, QByteArray> headers;

};





class HttpProxy : public BSocket
{
	Q_OBJECT

	QNetworkProxy proxy;

public:
	HttpProxy(const QNetworkProxy &proxy) :
		proxy(proxy) {}

	void get(){}
};

#if 0

class HttpClient : public QObject
{
	Q_OBJECT

	BSocket *bs;
	QNetworkProxy proxy;
	QCA::TLS *tls;

public:
	HttpClient(QObject *parent) :
		QObject(parent),
		bs(0),
		tls(0)
	{

	}

	void setProxy(const QNetworkProxy &proxy)
	{
		this->proxy = proxy;
	}

	void connect(const QUrl &url)
	{
		switch (this->proxy.type()) {
		case QNetworkProxy::Socks5Proxy:
		case QNetworkProxy::HttpProxy:
		case QNetworkProxy::HttpCachingProxy:
			Q_ASSERT("proxy connection is not implemented");
			break;
		default:
			bs = new BSocket(this);

			break;
		}
	}

private slots:
	void bs_connected()
	{
		tls = new QCA::TLS(this);
		connect(tls, SIGNAL(handshaken()), SLOT(tls_handshaken()));
		connect(tls, SIGNAL(readyRead()), SLOT(tls_readyRead()));
		connect(tls, SIGNAL(readyReadOutgoing()), SLOT(tls_readyReadOutgoing()));
		connect(tls, SIGNAL(error()), SLOT(tls_error()));
		tlsHandshaken = false;
	}

	void tls_handshaken()
	{
		tlsHandshaken = true;

		ObjectSessionWatcher watch(&sess);
		emit q->tlsHandshaken();
		if(!watch.isValid())
			return;

		tls->continueAfterStep();
		after_connected();
	}

	void tls_readyRead()
	{
		processStream(tls->read());
	}

	void tls_readyReadOutgoing()
	{
		bs->write(tls->readOutgoing());
	}

	void tls_closed()
	{
		delete tls;
		tls = 0;

		do_sock_close();
	}

	void tls_error()
	{
		cleanup();
		errorString = "TLS error.";
		emit q->error(TurnClient::ErrorTls);
	}
};

//--------------------------------------------
// NetworkSocketFactoryResult
//--------------------------------------------
class NetworkSocketFactoryResultInternal : public NetworkSocketFactoryResult
{
	Q_OBJECT

public:
	bool secure;
	QUrl url;
public:
	NetworkSocketFactoryResultInternal(const QUrl &url, QObject *parent=0) :
		NetworkSocketFactoryResult(parent),
		secure(url.scheme() == "https"),
		url(url)
	{
		BSocket *sock = new BSocket;
		connect(sock, SIGNAL(connected()), SLOT(sock_connected()));
		sock->connectToHost(url.host(), url.port() == -1? 80 : url.port());
	}

private slots:
	void sock_connected()
	{
		setSocket(reinterpret_cast<BSocket *>(sender()));
	}
};

NetworkSocketFactoryResult* NetworkSocketFactory::socket(const QUrl &url)
{
	return new NetworkSocketFactoryResultInternal(url);
}

//--------------------------------------------
// NetworkAccessManagerPrivate
//--------------------------------------------
class NetworkAccessManagerPrivate : public QObject
{
	Q_OBJECT

public:
	NetworkAccessManager *q;
	NetworkSocketFactory *socketFactory;
	NetworkSocketFactoryResult *socketFactoryResult;
	QNetworkDiskCache cache;

public:
	NetworkAccessManagerPrivate(NetworkAccessManager *nam) :
		q(nam),
		socketFactory(NULL)
	{}

	~NetworkAccessManagerPrivate()
	{
		delete socketFactory;
	}

	QNetworkReply* createRequest(QNetworkAccessManager::Operation op,
								 const QNetworkRequest &req,
								 QIODevice *outgoingData)
	{
		if (!socketFactory) {
			socketFactory = new NetworkSocketFactory;
		}
		socketFactoryResult = socketFactory->socket(req.url());
		Http::NetworkReply *reply = new Http::NetworkReply(socketFactoryResult, socketFactoryResult->url, sock);
		//connect(socketFactoryResult, SIGNAL(socketConnected()), SLOT(socketConnected()));
		return reply;
	}

private slots:
	void socketConnected()
	{
		ByteStream *sock = socketFactoryResult->socket();
		//reply = new Http::NetworkReply(socketFactoryResult->url, sock);
		delete socketFactoryResult;
		//connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead))
	}
};

//--------------------------------------------
// NetworkAccessManager
//--------------------------------------------
NetworkAccessManager::NetworkAccessManager(QObject *parent) :
	QNetworkAccessManager(parent),
	d(new NetworkAccessManagerPrivate(this))
{
}

NetworkAccessManager::~NetworkAccessManager()
{
	delete d;
}

QNetworkDiskCache &NetworkAccessManager::cache() const
{
	return d->cache;
}

void NetworkAccessManager::setSocketFactory(NetworkSocketFactory *factory)
{
	if (d->socketFactory) {
		delete d->socketFactory;
	}
	d->socketFactory = factory;
}

QNetworkReply* NetworkAccessManager::createRequest(
		Operation op, const QNetworkRequest &req, QIODevice *outgoingData)
{
	if (req.url().scheme() != "https" && req.url().scheme() != "http") {
		return createRequest(op, req, outgoingData);
	}
	// only http(s) here
	return d->createRequest(op, req, outgoingData);
}

#endif

#include "networkaccessmanager.moc"
