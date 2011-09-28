#include "QWsServer.h"

#include <QTcpSocket>
#include <QRegExp>
#include <QStringList>
#include <QString>
#include <QByteArray>
#include <QCryptographicHash>

const QString QWsServer::regExpResourceNameStr( "GET\\s(.*)\\sHTTP/1.1\r\n");
const QString QWsServer::regExpHostStr( "Host:\\s(.+:\\d+)\r\n" );
const QString QWsServer::regExpKeyStr( "Sec-WebSocket-Key:\\s(.{24})\r\n" );
const QString QWsServer::regExpVersionStr( "Sec-WebSocket-Version:\\s(\\d)\r\n" );
const QString QWsServer::regExpOriginStr( "Sec-WebSocket-Origin:\\s(.+)\r\n" );
const QString QWsServer::regExpProtocolStr( "Sec-WebSocket-Protocol:\\s(.+)\r\n" );
const QString QWsServer::regExpExtensionsStr( "Sec-WebSocket-Extensions:\\s(.+)\r\n" );

QWsServer::QWsServer(QObject * parent)
	: QTcpServer(parent)
{
    tcpServer = new QTcpServer(this);
	connect(tcpServer, SIGNAL(newConnection()), this, SLOT(newTcpConnection()));
}

QWsServer::~QWsServer()
{
	tcpServer->deleteLater();
}

bool QWsServer::listen(const QHostAddress & address, quint16 port)
{
	bool launched = tcpServer->listen(address, port);

	if ( ! launched )
		treatSocketError();

	return launched;
}

void QWsServer::close()
{
	tcpServer->close();
}

void QWsServer::treatSocketError()
{
	serverSocketError = tcpServer->serverError();
	serverSocketErrorString = tcpServer->errorString();
}

QAbstractSocket::SocketError QWsServer::serverError()
{
	return serverSocketError;
}

QString QWsServer::errorString()
{
	return serverSocketErrorString;
}

void QWsServer::newTcpConnection()
{
    QTcpSocket * clientSocket = tcpServer->nextPendingConnection();

	QObject * clientObject = qobject_cast<QObject*>(clientSocket);

    connect(clientObject, SIGNAL(readyRead()), this, SLOT(dataReceived()));
    connect(clientObject, SIGNAL(disconnected()), this, SLOT(clientDisconnected()));
}

void QWsServer::clientDisconnected()
{
    QTcpSocket * clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (clientSocket == 0)
        return;
}

void QWsServer::dataReceived()
{
    QTcpSocket * clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (clientSocket == 0)
        return;

	QString request( clientSocket->readAll() );

	QRegExp regExp;
	regExp.setMinimal( true );
	
	// Extract mandatory datas
	// Resource name
	regExp.setPattern( QWsServer::regExpResourceNameStr );
	regExp.indexIn(request);
	resourceName = regExp.cap(1);
	
	// Host (address & port)
	regExp.setPattern( QWsServer::regExpHostStr );
	regExp.indexIn(request);
	QStringList sl = regExp.cap(1).split(':');
	QString hostPort;
	if ( sl.size() > 1 )
		hostPort = sl[1];
	QString hostAddress = sl[0];
	
	// Key
	regExp.setPattern( QWsServer::regExpKeyStr );
	regExp.indexIn(request);
	QString key = regExp.cap(1);
	
	// Version
	regExp.setPattern( QWsServer::regExpVersionStr );
	regExp.indexIn(request);
	QString version = regExp.cap(1);
	
	// Extract optional datas
	// Origin
	regExp.setPattern( QWsServer::regExpOriginStr );
	regExp.indexIn(request);
	QString origin = regExp.cap(1);

	regExp.setPattern( QWsServer::regExpProtocolStr );
	regExp.indexIn(request);
	QString protocol = regExp.cap(1);

	regExp.setPattern( QWsServer::regExpExtensionsStr );
	regExp.indexIn(request);
	QString extensions = regExp.cap(1);

	// If the mandatory params are not setted, we abord the handshake
	if ( hostAddress.isEmpty()
		|| hostPort.isEmpty()
		|| resourceName.isEmpty()
		|| key.isEmpty()
		|| version != "8" )
		return;

	incomingConnection( clientSocket->socketDescriptor() );

	// Compose answer
	QString accept = computeAcceptV8( key );
	
	QString answer("HTTP/1.1 101 Switching Protocols\r\n");
	answer.append("Upgrade: websocket\r\n");
	answer.append("Connection: Upgrade\r\n");
	answer.append("Sec-WebSocket-Accept: " + accept + "\r\n");
	answer.append("\r\n");

	clientSocket->write( answer.toUtf8() );
}

QString QWsServer::computeAcceptV8(QString key)
{
	key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	QByteArray hash = QCryptographicHash::hash ( key.toUtf8(), QCryptographicHash::Sha1 );
	return hash.toBase64();
}

void QWsServer::addPendingConnection( QTcpSocket * socket )
{
	if ( pendingConnections.size() < QTcpServer::maxPendingConnections() )
		pendingConnections.enqueue(socket);
}

void QWsServer::incomingConnection( int socketDescriptor )
{
	QTcpSocket * socket = new QTcpSocket(tcpServer);
	socket->setSocketDescriptor( socketDescriptor );
	
	addPendingConnection( socket );

	emit QWsServer::newConnection();
}

bool QWsServer::hasPendingConnections()
{
	if ( pendingConnections.size() > 0 )
		return true;
	return false;
}

QTcpSocket * QWsServer::nextPendingConnection()
{
	return pendingConnections.dequeue();
}


QString QWsServer::decodeFrame( QTcpSocket * socket )
{
	QByteArray BA; // ReadBuffer
	quint8 byte; // currentByteBuffer

	// FIN, RSV1-3, Opcode
	BA = socket->read(1);
	byte = BA[0];
	quint8 FIN = (byte >> 7);
	quint8 RSV1 = ((byte & 0x7F) >> 6);
	quint8 RSV2 = ((byte & 0x3F) >> 5);
	quint8 RSV3 = ((byte & 0x1F) >> 4);
	quint8 Opcode = (byte & 0x0F);

	// Mask, PayloadLength
	BA = socket->read(1);
	byte = BA[0];
	quint8 Mask = (byte >> 7);
	quint8 PayloadLength = (byte & 0x7F);
	// Extended PayloadLength
	if ( PayloadLength == 126 )
	{
		BA = socket->read(2);
		//PayloadLength = (BA[0] + (BA[1] << 8));
		PayloadLength = BA.toUShort();
	}
	else if ( PayloadLength == 127 )
	{
		BA = socket->read(8);
		//PayloadLength = ((B[0] << 0*8) + (B[1] << 1*8) + (BA[2] << 2*8) + (BA[3] << 3*8) + (BA[4] << 4*8) + (BA[5] << 5*8) + (BA[6] << 6*8) + (BA[7] << 7*8));
		PayloadLength = BA.toULongLong();
	}

	// MaskingKey
	QByteArray MaskingKey;
	if ( Mask )
	{
		MaskingKey = socket->read(4);
	}

	// ExtensionData
	QByteArray ExtensionData;
	// Extension data is ignored for instance

	// ApplicationData
	QByteArray ApplicationData = socket->read( PayloadLength );
	if ( Mask )
	{
		for ( int i=0 ; i<PayloadLength ; i++ )
		{
			ApplicationData[i] = ( ApplicationData[i] ^ MaskingKey[ i % 4 ] );
		}
	}
	// Record ApplicationData

	return QString( ApplicationData );
}

QByteArray QWsServer::composeFrame( QString message, int maxFrameBytes )
{
	quint64 messageSize = message.size();
	QByteArray BA;
	quint8 byte;

	// FIN, RSV1-3, Opcode
	byte = 0x00;
	if ( message.size() < 126 )
	{
		// FIN
		byte = (byte | 0x80);
		// Opcode
		byte = (byte | 0x01);
	}
	else
	{
		// FIN (depending of the FrameNum)
		// Opcode = 0
	}
	// RSV1-3
	// for instance, do nothing
	BA.append( byte );

	// Mask, PayloadLength
	byte = 0x00;
	// Mask = 0
	// PayloadLength
	if ( messageSize < 126 )
	{
		quint8 sz = (quint8)messageSize;
		byte = (byte | sz);
		BA.append( byte );
	}
	// Extended payloadLength
	else
	{
		QByteArray BAtmp;
		// 2 bytes
		if ( messageSize <= 0xFFFF )
		{
			BAtmp = QByteArray::number( (quint16)messageSize );
		}
		// 8 bytes
		else if ( messageSize < 0xFFFF )
		{
			BAtmp = QByteArray::number( (quint64)messageSize );
		}
		// bug on most significant bit
		else
		{
			// BUG, STOP
			return QByteArray();
		}
		BA.append( BAtmp );
	}

	// Masking (ignored for instance)

	// Application Data
	BA.append( message.toAscii() );

	return BA;
}
