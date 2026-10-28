#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QUdpSocket>

#include <array>
#include <cmath>

namespace {
constexpr double kCenterLatitude = 19.0760;
constexpr double kCenterLongitude = 72.8777;
constexpr double kRadiusKm = 3000.0;
constexpr double kMaximumAltitudeMeters = 500.0;
constexpr qsizetype kMaximumDatagramBytes = 1400;
constexpr std::array<qsizetype, 3> kBatchSizes{50000, 100000, 150000};

struct Point {
	double latitude;
	double longitude;
	double altitude;
};

Point randomPoint() {
	constexpr double kEarthRadiusKm = 6371.0;
	constexpr double kPi = 3.14159265358979323846;
	auto* generator = QRandomGenerator::global();

	const double distanceKm = kRadiusKm * std::sqrt(generator->generateDouble());
	const double bearing = 2.0 * kPi * generator->generateDouble();
	const double latitudeOffset = distanceKm * std::cos(bearing) / kEarthRadiusKm;
	const double longitudeOffset = distanceKm * std::sin(bearing) /
								   (kEarthRadiusKm * std::cos(kCenterLatitude * kPi / 180.0));

	return {
		kCenterLatitude + latitudeOffset * 180.0 / kPi,
		kCenterLongitude + longitudeOffset * 180.0 / kPi,
		generator->generateDouble() * kMaximumAltitudeMeters,
	};
}

bool sendBatch(QUdpSocket& socket, const QHostAddress& destination, quint16 port,
			   qsizetype batchId, qsizetype pointCount, qsizetype& totalBytesSent) {
	qsizetype pointsSent = 0;
	totalBytesSent = 0;

	while (pointsSent < pointCount) {
		QByteArray datagram = QByteArray::number(batchId) + ',' +
							  QByteArray::number(pointCount) + ',' +
							  QByteArray::number(pointsSent) + '\n';
		qsizetype pointsInDatagram = 0;
		while (pointsSent + pointsInDatagram < pointCount) {
			const Point point = randomPoint();
			QByteArray record = QByteArray::number(point.latitude, 'f', 6) + ',' +
								QByteArray::number(point.longitude, 'f', 6) + ',' +
								QByteArray::number(point.altitude, 'f', 1) + '\n';
			if (datagram.size() + record.size() > kMaximumDatagramBytes) {
				break;
			}
			datagram += record;
			++pointsInDatagram;
		}

		const qint64 bytesSent = socket.writeDatagram(datagram, destination, port);
		if (bytesSent != datagram.size()) {
			QTextStream(stderr) << "Could not send UDP datagram: "
								<< socket.errorString() << Qt::endl;
			return false;
		}
		totalBytesSent += bytesSent;
		pointsSent += pointsInDatagram;
	}
	return true;
}
}  // namespace

int main(int argc, char* argv[]) {
	QCoreApplication application(argc, argv);
	const QStringList arguments = application.arguments();
	if (arguments.size() != 3) {
		QTextStream(stderr) << "Usage: " << arguments[0]
							<< " <destination-ipv4> <port>\n";
		return 1;
	}

	QHostAddress destination;
	if (!destination.setAddress(arguments[1]) ||
		destination.protocol() != QAbstractSocket::IPv4Protocol) {
		QTextStream(stderr) << "Destination must be a valid IPv4 address\n";
		return 1;
	}

	bool validPort = false;
	const int parsedPort = arguments[2].toInt(&validPort);
	if (!validPort || parsedPort < 1 || parsedPort > 65535) {
		QTextStream(stderr) << "Port must be an integer between 1 and 65535\n";
		return 1;
	}

	QUdpSocket socket;
	qsizetype batchId = 0;
	auto sendNextBatch = [&]() {
		const qsizetype pointCount = kBatchSizes[batchId % kBatchSizes.size()];
		qsizetype totalBytesSent = 0;
		if (!sendBatch(socket, destination, static_cast<quint16>(parsedPort),
					   batchId, pointCount, totalBytesSent)) {
			return false;
		}

		QTextStream(stdout)
			<< QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
			<< " Sent batch " << batchId << " with " << pointCount << " points, "
			<< totalBytesSent << " bytes" << Qt::endl;
		++batchId;
		return true;
	};

	QTimer timer;
	timer.setInterval(5000);
	QObject::connect(&timer, &QTimer::timeout, &application, [&]() {
		if (!sendNextBatch()) {
			timer.stop();
			application.exit(1);
		}
	});
	timer.start();

	if (!sendNextBatch()) {
		return 1;
	}
	return application.exec();
}
