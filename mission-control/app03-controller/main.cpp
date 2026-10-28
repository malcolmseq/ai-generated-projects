#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QTcpServer>
#include <QTextStream>
#include <QUdpSocket>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr int kDefaultPort = 8080;
constexpr int kDefaultMapUdpPort = 1235;
constexpr int kMinimumZoom = 0;
constexpr int kMaximumZoom = 8;
constexpr int kTileSize = 256;
constexpr double kPanPixels = 256.0;
constexpr double kPi = 3.14159265358979323846;
constexpr qint64 kMaximumMapFileSize = 32 * 1024 * 1024;

struct MapView {
	int zoom = 7;
	double latitude = 19.076090;
	double longitude = 72.877426;
};

bool parsePort(const QString& value, int& port) {
	bool valid = false;
	const int parsedPort = value.toInt(&valid);
	if (!valid || parsedPort < 1 || parsedPort > 65535) {
		return false;
	}
	port = parsedPort;
	return true;
}

double wrappedLongitude(double longitude) {
	longitude = std::fmod(longitude + 180.0, 360.0);
	if (longitude < 0.0) {
		longitude += 360.0;
	}
	return longitude - 180.0;
}

void applyControlRoute(const QString& path, MapView& view) {
	if (path == "/zoom/increment") {
		view.zoom = std::min(kMaximumZoom, view.zoom + 1);
		return;
	}
	if (path == "/zoom/decrement") {
		view.zoom = std::max(kMinimumZoom, view.zoom - 1);
		return;
	}

	const double worldSize = kTileSize * static_cast<double>(1 << view.zoom);
	const double latitudeRadians = view.latitude * kPi / 180.0;
	double centerX = (view.longitude + 180.0) / 360.0 * worldSize;
	double centerY =
		(1.0 - std::asinh(std::tan(latitudeRadians)) / kPi) / 2.0 * worldSize;

	if (path == "/pan/top") {
		centerY -= kPanPixels;
	} else if (path == "/pan/bottom") {
		centerY += kPanPixels;
	} else if (path == "/pan/right") {
		centerX += kPanPixels;
	} else {
		centerX -= kPanPixels;
	}

	const double clampedCenterY = std::clamp(centerY, 0.0, worldSize);
	const double mercatorY = kPi * (1.0 - 2.0 * clampedCenterY / worldSize);
	view.latitude = std::atan(std::sinh(mercatorY)) * 180.0 / kPi;
	view.longitude = wrappedLongitude(centerX / worldSize * 360.0 - 180.0);
}

QHttpServerResponse textResponse(const QByteArray& body,
								 QHttpServerResponse::StatusCode status) {
	return QHttpServerResponse("text/plain; charset=utf-8", body, status);
}

bool isGetOrHead(const QHttpServerRequest& request) {
	return request.method() == QHttpServerRequest::Method::Get ||
		   request.method() == QHttpServerRequest::Method::Head;
}

QHttpServerResponse pageResponse(const QString& resourcePath,
								 const QHttpServerRequest& request) {
	if (!isGetOrHead(request)) {
		return textResponse("Method Not Allowed\n",
							QHttpServerResponse::StatusCode::MethodNotAllowed);
	}

	QFile file(resourcePath);
	if (!file.open(QIODevice::ReadOnly)) {
		qWarning() << "Could not read embedded page" << resourcePath << file.errorString();
		return textResponse("Internal Server Error\n",
							QHttpServerResponse::StatusCode::InternalServerError);
	}
	return QHttpServerResponse("text/html; charset=utf-8", file.readAll());
}

QHttpServerResponse mapImageResponse(const QHttpServerRequest& request) {
	if (!isGetOrHead(request)) {
		return textResponse("Method Not Allowed\n",
							QHttpServerResponse::StatusCode::MethodNotAllowed);
	}

	QFile file(QString::fromLocal8Bit(APP03_MAP_IMAGE_PATH));
	if (!file.open(QIODevice::ReadOnly)) {
		qWarning() << "Could not open map image:" << file.fileName() << file.errorString();
		return textResponse("Not Found\n", QHttpServerResponse::StatusCode::NotFound);
	}
	if (file.size() > kMaximumMapFileSize) {
		return textResponse("Content Too Large\n",
							static_cast<QHttpServerResponse::StatusCode>(413));
	}

	const QByteArray image = file.readAll();
	if (file.error() != QFileDevice::NoError) {
		qWarning() << "Could not read map image:" << file.fileName() << file.errorString();
		return textResponse("Internal Server Error\n",
							QHttpServerResponse::StatusCode::InternalServerError);
	}
	return QHttpServerResponse("image/png", image);
}

QHttpServerResponse controlResponse(const QString& path, const QHttpServerRequest& request,
									QUdpSocket& socket, quint16 udpPort, MapView& view) {
	if (request.method() != QHttpServerRequest::Method::Get) {
		return textResponse("Method Not Allowed\n",
							QHttpServerResponse::StatusCode::MethodNotAllowed);
	}

	MapView updatedView = view;
	applyControlRoute(path, updatedView);
	const QByteArray payload =
		QByteArray::number(updatedView.zoom) + ',' +
		QByteArray::number(updatedView.latitude, 'f', 6) + ',' +
		QByteArray::number(updatedView.longitude, 'f', 6);
	if (socket.writeDatagram(payload, QHostAddress::LocalHost, udpPort) != payload.size()) {
		qWarning() << "Could not send map view over UDP:" << socket.errorString();
		return textResponse("Bad Gateway\n", QHttpServerResponse::StatusCode::BadGateway);
	}

	view = updatedView;
	return QHttpServerResponse(QHttpServerResponse::StatusCode::Ok);
}

int runServer(int port, int mapUdpPort) {
	QHttpServer server;
	QUdpSocket udpSocket;
	MapView mapView;

	server.route("/index", [](const QHttpServerRequest& request) {
		return pageResponse(":/web/index.html", request);
	});
	server.route("/index.html", [](const QHttpServerRequest& request) {
		return pageResponse(":/web/index.html", request);
	});
	server.route("/controller", [](const QHttpServerRequest& request) {
		return pageResponse(":/web/controller.html", request);
	});
	server.route("/controller.html", [](const QHttpServerRequest& request) {
		return pageResponse(":/web/controller.html", request);
	});
	server.route("/build/test.png", [](const QHttpServerRequest& request) {
		return mapImageResponse(request);
	});

	const std::array<QString, 6> controlPaths{
		"/zoom/increment", "/zoom/decrement", "/pan/top",
		"/pan/bottom", "/pan/right", "/pan/left"};
	for (const QString& path : controlPaths) {
		server.route(path, [&udpSocket, &mapView, path, mapUdpPort](
							   const QHttpServerRequest& request) {
			return controlResponse(path, request, udpSocket,
								   static_cast<quint16>(mapUdpPort), mapView);
		});
	}

	QTcpServer tcpServer;
	if (!tcpServer.listen(QHostAddress::Any, static_cast<quint16>(port)) ||
		!server.bind(&tcpServer)) {
		qCritical() << "Could not start HTTP server on port" << port
					<< tcpServer.errorString();
		return 1;
	}

	QTextStream(stdout) << "Serving controller pages at http://localhost:" << port
						<< "/index and http://localhost:" << port << "/controller\n"
						<< "Sending map updates to UDP port " << mapUdpPort << Qt::endl;
	return QCoreApplication::exec();
}
}  // namespace

int main(int argc, char* argv[]) {
	QCoreApplication application(argc, argv);
	const QStringList arguments = application.arguments();
	if (arguments.size() > 3) {
		QTextStream(stderr) << "Usage: " << arguments[0]
							<< " [http-port] [map-udp-port]\n";
		return 1;
	}

	int port = kDefaultPort;
	int mapUdpPort = kDefaultMapUdpPort;
	if ((arguments.size() >= 2 && !parsePort(arguments[1], port)) ||
		(arguments.size() == 3 && !parsePort(arguments[2], mapUdpPort))) {
		QTextStream(stderr) << "Ports must be integers between 1 and 65535\n";
		return 1;
	}

	return runServer(port, mapUdpPort);
}
