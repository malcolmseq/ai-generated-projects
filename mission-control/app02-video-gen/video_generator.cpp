#include "video_generator.h"

#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QString>
#include <QUdpSocket>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr std::size_t kMaximumRowsPerTransmission = 150000;
constexpr std::size_t kMaximumActiveTransmissions = 8;
constexpr auto kTransmissionTimeout = std::chrono::seconds(30);
constexpr int kTileSize = 256;
constexpr int kMapWidth = 1920;
constexpr int kMapHeight = 1080;
constexpr int kMinimumZoom = 0;
constexpr int kMaximumZoom = 8;
constexpr double kMaximumMercatorLatitude = 85.0511287798066;
constexpr double kPi = 3.14159265358979323846;
constexpr auto kPollTimeoutMilliseconds = 250;

struct Transmission {
	std::vector<Point> points;
	std::vector<unsigned char> received;
	std::size_t receivedRows = 0;
	std::chrono::steady_clock::time_point lastActivity;
};

struct PixelCoordinate {
	double x;
	double y;
};


bool isMissingTileError(const std::error_code& error) {
	return error == std::errc::no_such_file_or_directory ||
		   error == std::errc::not_a_directory;
}

std::string currentTimestamp() {
	const auto now = std::chrono::system_clock::now();
	const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()) % 1000;
	const std::time_t time = std::chrono::system_clock::to_time_t(now);
	std::tm localTime{};
	localtime_r(&time, &localTime);

	std::ostringstream timestamp;
	timestamp << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << '.'
			  << std::setfill('0') << std::setw(3) << milliseconds.count();
	return timestamp.str();
}

bool validCoordinates(double latitude, double longitude) {
	return std::isfinite(latitude) && std::isfinite(longitude) &&
		   latitude >= -90.0 && latitude <= 90.0 &&
		   longitude >= -180.0 && longitude <= 180.0;
}

bool parseHeader(const std::string& line, std::size_t& batchId,
				 std::size_t& totalPoints, std::size_t& pointOffset) {
	std::istringstream input(line);
	char firstSeparator = '\0';
	char secondSeparator = '\0';
	if (!(input >> batchId >> firstSeparator >> totalPoints >> secondSeparator >> pointOffset) ||
		firstSeparator != ',' || secondSeparator != ',') {
		return false;
	}
	input >> std::ws;
	return input.eof();
}

bool parsePoint(const std::string& line, Point& point) {
	std::istringstream input(line);
	char firstSeparator = '\0';
	char secondSeparator = '\0';
	if (!(input >> point.latitude >> firstSeparator >> point.longitude >> secondSeparator >>
		  point.altitude) ||
		firstSeparator != ',' || secondSeparator != ',' ||
		!validCoordinates(point.latitude, point.longitude) || !std::isfinite(point.altitude)) {
		return false;
	}
	input >> std::ws;
	return input.eof();
}

bool parseMapView(const std::string& payload, int& zoom, double& latitude, double& longitude) {
	std::istringstream input(payload);
	char firstSeparator = '\0';
	char secondSeparator = '\0';
	if (!(input >> zoom >> firstSeparator >> latitude >> secondSeparator >> longitude) ||
		firstSeparator != ',' || secondSeparator != ',' ||
		zoom < kMinimumZoom || zoom > kMaximumZoom ||
		!validCoordinates(latitude, longitude)) {
		return false;
	}
	input >> std::ws;
	return input.eof();
}

PixelCoordinate project(double latitude, double longitude, int zoom) {
	const double worldSize = static_cast<double>(kTileSize * (1 << zoom));
	const double clampedLatitude =
		std::clamp(latitude, -kMaximumMercatorLatitude, kMaximumMercatorLatitude);
	const double latitudeRadians = clampedLatitude * kPi / 180.0;
	return {
		(longitude + 180.0) / 360.0 * worldSize,
		(1.0 - std::asinh(std::tan(latitudeRadians)) / kPi) / 2.0 * worldSize,
	};
}

}  // namespace

bool VideoGenerator::bindUdpSocket(QUdpSocket& socket, int port) {
	if (port < 1 || port > 65535) {
		std::cerr << "Invalid UDP port value " << port
				  << ": expected a value between 1 and 65535\n";
		return false;
	}

	if (!socket.bind(QHostAddress::AnyIPv4, static_cast<quint16>(port))) {
		std::cerr << "Could not bind UDP socket to port " << port << ".\n"
				  << "Qt error: " << socket.errorString().toStdString() << '\n'
				  << "Common causes: another process is using this port, the port is blocked, or "
				  << "the OS refuses the bind request.\n";
		return false;
	}

	std::cout << "Bound UDP socket successfully on port " << port << '\n';
	return true;
}

VideoGenerator::VideoGenerator(int pointsPort, int mapPort,
							   std::filesystem::path tilesDirectory,
							   std::filesystem::path outputPath)
	: pointsPort_(pointsPort),
	  mapPort_(mapPort),
	  tilesDirectory_(std::move(tilesDirectory)),
	  outputPath_(std::move(outputPath)) {
	if (pointsPort_ == mapPort_) {
		std::cerr << "Startup error: the points UDP port and map UDP port are the same ("
				  << pointsPort_ << "). They must be different.\n";
		failed_ = true;
	}
	if (!bindUdpSocket(pointsSocket_, pointsPort_)) {
		std::cerr << "Startup error: failed to bind the points UDP socket on port "
				  << pointsPort_ << '\n';
		failed_ = true;
	}
	if (!bindUdpSocket(mapSocket_, mapPort_)) {
		std::cerr << "Startup error: failed to bind the map UDP socket on port "
				  << mapPort_ << '\n';
		failed_ = true;
	}
	if (!failed_) {
		connect(&pointsSocket_, &QUdpSocket::readyRead, this, &VideoGenerator::onPointsReadyRead);
		connect(&mapSocket_, &QUdpSocket::readyRead, this, &VideoGenerator::onMapReadyRead);
		connect(&renderTimer_, &QTimer::timeout, this, &VideoGenerator::renderNextFrame);
		renderTimer_.setInterval(100);
	}
}

bool VideoGenerator::run() {
	if (failed_) {
		std::cerr << "VideoGenerator startup failed before the event loop began.\n"
				  << "Point socket open = " << pointsSocket_.isOpen() << ", "
				  << "map socket open = " << mapSocket_.isOpen() << '\n';
		return false;
	}
	std::cout << "Listening for UDP point batches on port " << pointsPort_ << '\n'
				  << "Listening for map updates (zoom,latitude,longitude) on port " << mapPort_
				  << '\n';
	renderTimer_.start();
	return QCoreApplication::exec() == 0 && !failed_;
}

void VideoGenerator::onPointsReadyRead() {
	static std::unordered_map<std::size_t, Transmission> transmissions;
	while (pointsSocket_.hasPendingDatagrams()) {
		QByteArray datagram;
		datagram.resize(static_cast<int>(pointsSocket_.pendingDatagramSize()));
		QHostAddress senderAddress;
		quint16 senderPort = 0;
		const qint64 bytesReceived = pointsSocket_.readDatagram(datagram.data(), datagram.size(),
													 &senderAddress, &senderPort);
		if (bytesReceived < 0) {
			std::cerr << currentTimestamp() << " readDatagram on point port failed: "
					  << pointsSocket_.errorString().toStdString() << '\n';
			failed_ = true;
			QCoreApplication::quit();
			return;
		}

		const std::string payload(datagram.constData(), static_cast<std::size_t>(bytesReceived));
		std::istringstream packet(payload);
		std::string header;
		std::size_t batchId = 0;
		std::size_t totalPoints = 0;
		std::size_t pointOffset = 0;
		if (!std::getline(packet, header) ||
			!parseHeader(header, batchId, totalPoints, pointOffset) || totalPoints == 0 ||
			totalPoints > kMaximumRowsPerTransmission || pointOffset >= totalPoints) {
			std::cerr << currentTimestamp() << " Ignoring malformed point datagram from "
					  << senderAddress.toString().toStdString() << ':' << senderPort << '\n';
			continue;
		}

		std::vector<Point> packetPoints;
		std::string pointLine;
		bool validPacket = true;
		while (std::getline(packet, pointLine)) {
			if (pointLine.empty()) {
				continue;
			}
			Point point{};
			if (!parsePoint(pointLine, point)) {
				validPacket = false;
				break;
			}
			packetPoints.push_back(point);
		}

		if (!validPacket || packetPoints.empty() ||
			packetPoints.size() > totalPoints - pointOffset) {
			std::cerr << currentTimestamp() << " Ignoring malformed datagram for batch "
					  << batchId << '\n';
			continue;
		}

		const auto now = std::chrono::steady_clock::now();
		for (auto transmission = transmissions.begin(); transmission != transmissions.end();) {
			if (now - transmission->second.lastActivity > kTransmissionTimeout) {
				transmission = transmissions.erase(transmission);
			} else {
				++transmission;
			}
		}

		auto transmission = transmissions.find(batchId);
		if (transmission == transmissions.end()) {
			if (transmissions.size() >= kMaximumActiveTransmissions) {
				std::cerr << currentTimestamp()
						  << " Too many incomplete transmissions; ignoring batch " << batchId
						  << '\n';
				continue;
			}
			Transmission newTransmission;
			newTransmission.points.resize(totalPoints);
			newTransmission.received.resize(totalPoints, 0);
			newTransmission.lastActivity = now;
			transmission = transmissions.emplace(batchId, std::move(newTransmission)).first;
		}

		Transmission& batch = transmission->second;
		if (batch.points.size() != totalPoints) {
			std::cerr << currentTimestamp() << " Conflicting row count for batch " << batchId
					  << '\n';
			continue;
		}
		batch.lastActivity = now;
		for (std::size_t index = 0; index < packetPoints.size(); ++index) {
			const std::size_t row = pointOffset + index;
			if (batch.received[row] == 0) {
				batch.points[row] = packetPoints[index];
				batch.received[row] = 1;
				++batch.receivedRows;
			}
		}

		if (batch.receivedRows == totalPoints) {
			std::cout << currentTimestamp() << " Transmission complete: batch " << batchId
					  << ", " << batch.receivedRows << " rows\n";
			publishPoints(std::move(batch.points));
			transmissions.erase(transmission);
		}
	}
}

void VideoGenerator::onMapReadyRead() {
	while (mapSocket_.hasPendingDatagrams()) {
		QByteArray datagram;
		datagram.resize(static_cast<int>(mapSocket_.pendingDatagramSize()));
		QHostAddress senderAddress;
		quint16 senderPort = 0;
		const qint64 bytesReceived = mapSocket_.readDatagram(datagram.data(), datagram.size(),
												 &senderAddress, &senderPort);
		if (bytesReceived < 0) {
			std::cerr << currentTimestamp() << " readDatagram on map port failed: "
					  << mapSocket_.errorString().toStdString() << '\n';
			failed_ = true;
			QCoreApplication::quit();
			return;
		}

		const std::string payload(datagram.constData(), static_cast<std::size_t>(bytesReceived));
		MapView view;
		if (!parseMapView(payload, view.zoom, view.latitude, view.longitude)) {
			std::cerr << currentTimestamp()
					  << " Ignoring malformed map packet; expected zoom,latitude,longitude "
						 "(zoom 0-7)\n";
			continue;
		}
		publishMapView(view);
		std::cout << currentTimestamp() << " Map view updated: zoom " << view.zoom
					  << ", center " << view.latitude << ',' << view.longitude << '\n';
	}
}

void VideoGenerator::publishPoints(std::vector<Point> points) {
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		points_ = std::move(points);
		++revision_;
	}
	if (renderTimer_.isActive()) {
		renderTimer_.start();
	}
}

void VideoGenerator::publishMapView(const MapView& view) {
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		mapView_ = view;
		++revision_;
	}
	if (renderTimer_.isActive()) {
		renderTimer_.start();
	}
}

void VideoGenerator::renderNextFrame() {
	std::vector<Point> points;
	MapView view;
	std::size_t currentRevision = 0;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		if (revision_ == renderedRevision_) {
			return;
		}
		points = points_;
		view = mapView_;
		currentRevision = revision_;
	}

	if (renderMap(points, view)) {
		renderedRevision_ = currentRevision;
		std::cout << currentTimestamp() << " Wrote " << points.size()
			  << " points to PNG " << outputPath_ << '\n';
	} else {
		failed_ = true;
		QCoreApplication::quit();
	}
}

bool VideoGenerator::renderMap(const std::vector<Point>& points, const MapView& view) const {
	const PixelCoordinate center = project(view.latitude, view.longitude, view.zoom);
	const double originX = center.x - kMapWidth / 2.0;
	const double originY = center.y - kMapHeight / 2.0;

	std::filesystem::path temporaryPath = outputPath_;
	temporaryPath += ".tmp";
	const auto removeTemporaryOutput = [&] {
		std::error_code ignored;
		std::filesystem::remove(temporaryPath, ignored);
	};

	int tileZoom = std::min(view.zoom, kMaximumZoom);
	auto tileCountForZoom = [](int zoom) { return 1 << zoom; };
	auto tilePathFor = [&](int zoom, int tileX, int tileY) {
		const int tileCount = tileCountForZoom(zoom);
		const int wrappedTileX = ((tileX % tileCount) + tileCount) % tileCount;
		return tilesDirectory_ /
			   (std::to_string(zoom) + "/" +
				std::to_string(wrappedTileX) + "/" + std::to_string(tileY) + ".png");
	};

	bool hasVisibleTile = false;
	for (; tileZoom >= kMinimumZoom; --tileZoom) {
		const int scale = 1 << (view.zoom - tileZoom);
		const int tileSize = kTileSize * scale;
		const int firstTileX = static_cast<int>(std::floor(originX / tileSize));
		const int firstTileY = static_cast<int>(std::floor(originY / tileSize));
		const int visibleColumns = kMapWidth / tileSize + 1;
		const int visibleRows = kMapHeight / tileSize + 1;
		bool found = false;
		for (int row = 0; row <= visibleRows && !found; ++row) {
			const int tileY = firstTileY + row;
			if (tileY < 0 || tileY >= tileCountForZoom(tileZoom)) {
				continue;
			}
			for (int column = 0; column <= visibleColumns; ++column) {
				const auto tilePath = tilePathFor(tileZoom, firstTileX + column, tileY);
				std::error_code filesystemError;
				if (std::filesystem::is_regular_file(tilePath, filesystemError)) {
					found = true;
					break;
				}
				if (filesystemError && !isMissingTileError(filesystemError)) {
					std::cerr << currentTimestamp() << " Could not inspect map tile " << tilePath
							  << ": " << filesystemError.message() << '\n';
					return false;
				}
			}
		}
		if (found) {
			hasVisibleTile = true;
			break;
		}
	}
	if (!hasVisibleTile) {
		std::cerr << currentTimestamp() << " No local map tiles found under "
					  << tilesDirectory_ << " for the requested map view\n";
		return false;
	}

	if (tileZoom < view.zoom) {
		std::cerr << currentTimestamp() << " Using zoom " << tileZoom
					  << " local tiles scaled for requested zoom " << view.zoom << '\n';
	}

	const int tileScale = 1 << (view.zoom - tileZoom);
	const int tileSize = kTileSize * tileScale;
	const int tileCount = tileCountForZoom(tileZoom);
	const int firstTileX = static_cast<int>(std::floor(originX / tileSize));
	const int firstTileY = static_cast<int>(std::floor(originY / tileSize));
	const int visibleColumns = kMapWidth / tileSize + 1;
	const int visibleRows = kMapHeight / tileSize + 1;
	int renderedTiles = 0;
	int missingTiles = 0;

	QImage mapImage(kMapWidth, kMapHeight, QImage::Format_ARGB32_Premultiplied);
	mapImage.fill(QColor(230, 230, 230));
	QPainter painter(&mapImage);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, false);

	for (int row = 0; row <= visibleRows; ++row) {
		const int tileY = firstTileY + row;
		if (tileY < 0 || tileY >= tileCount) {
			continue;
		}
		for (int column = 0; column <= visibleColumns; ++column) {
			const int tileX = firstTileX + column;
			const std::filesystem::path tilePath = tilePathFor(tileZoom, tileX, tileY);
			std::error_code filesystemError;
			if (!std::filesystem::is_regular_file(tilePath, filesystemError)) {
				if (filesystemError && !isMissingTileError(filesystemError)) {
					std::cerr << currentTimestamp() << " Could not inspect map tile " << tilePath
							  << ": " << filesystemError.message() << '\n';
					return false;
				}
				++missingTiles;
				continue;
			}

			QImage tileImage(QString::fromStdString(tilePath.string()));
			if (tileImage.isNull()) {
				std::cerr << currentTimestamp() << " Could not decode map tile " << tilePath
					  << '\n';
				return false;
			}
			if (tileImage.width() != kTileSize || tileImage.height() != kTileSize) {
				std::cerr << currentTimestamp() << " Unexpected dimensions for map tile "
					  << tilePath << ": " << tileImage.width() << 'x' << tileImage.height()
					  << '\n';
				return false;
			}

			const double tileLeft = tileX * tileSize - originX;
			const double tileTop = tileY * tileSize - originY;
			painter.drawImage(QRectF(tileLeft, tileTop, tileSize, tileSize), tileImage);
			++renderedTiles;
		}
	}
	if (renderedTiles == 0) {
		std::cerr << currentTimestamp() << " No local map tiles found under "
					  << tilesDirectory_ << " for zoom " << tileZoom << '\n';
		return false;
	}
	if (missingTiles > 0) {
		std::cerr << currentTimestamp() << " " << missingTiles
					  << " visible local map tiles are missing for zoom " << tileZoom << '\n';
	}

	for (const Point& point : points) {
		PixelCoordinate projected = project(point.latitude, point.longitude, view.zoom);
		const int viewTileCount = tileCountForZoom(view.zoom);
		while (projected.x - center.x > (viewTileCount * kTileSize) / 2.0) {
			projected.x -= viewTileCount * kTileSize;
		}
		while (projected.x - center.x < -(viewTileCount * kTileSize) / 2.0) {
			projected.x += viewTileCount * kTileSize;
		}
		const int centerX = static_cast<int>(std::round(projected.x - originX));
		const int centerY = static_cast<int>(std::round(projected.y - originY));
		if (centerX < -7 || centerX >= kMapWidth + 7 ||
			centerY < -7 || centerY >= kMapHeight + 7) {
			continue;
		}

		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(255, 255, 255));
		painter.drawEllipse(QPoint(centerX, centerY), 7, 7);
		painter.setBrush(QColor(229, 57, 53));
		painter.drawEllipse(QPoint(centerX, centerY), 5, 5);
	}
	painter.end();

	if (!mapImage.save(QString::fromStdString(temporaryPath.string()), "PNG")) {
		std::cerr << currentTimestamp() << " Failed writing PNG map " << temporaryPath << '\n';
		removeTemporaryOutput();
		return false;
	}

	std::error_code error;
	std::filesystem::rename(temporaryPath, outputPath_, error);
	if (error) {
		std::cerr << currentTimestamp() << " Could not publish map output " << outputPath_
					  << ": " << error.message() << '\n';
		removeTemporaryOutput();
		return false;
	}
	return true;
}
