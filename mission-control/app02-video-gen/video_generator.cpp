#include "video_generator.h"

#include <arpa/inet.h>
#include <png.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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

std::array<unsigned char, 7> timestampGlyph(char character) {
	switch (character) {
	case '0': return {14, 17, 19, 21, 25, 17, 14};
	case '1': return {4, 12, 4, 4, 4, 4, 14};
	case '2': return {14, 17, 1, 2, 4, 8, 31};
	case '3': return {30, 1, 1, 14, 1, 1, 30};
	case '4': return {2, 6, 10, 18, 31, 2, 2};
	case '5': return {31, 16, 16, 30, 1, 1, 30};
	case '6': return {6, 8, 16, 30, 17, 17, 14};
	case '7': return {31, 1, 2, 4, 8, 8, 8};
	case '8': return {14, 17, 17, 14, 17, 17, 14};
	case '9': return {14, 17, 17, 15, 1, 2, 12};
	case '-': return {0, 0, 0, 31, 0, 0, 0};
	case ':': return {0, 4, 4, 0, 4, 4, 0};
	case '.': return {0, 0, 0, 0, 0, 6, 6};
	default: return {};
	}
}

void drawTimestamp(std::vector<png_byte>& pixels, const std::string& timestamp) {
	constexpr int scale = 3;
	constexpr int glyphWidth = 5;
	constexpr int glyphHeight = 7;
	constexpr int characterSpacing = 1;
	constexpr int paddingX = 12;
	constexpr int paddingY = 8;
	const int textWidth =
		static_cast<int>(timestamp.size()) * (glyphWidth + characterSpacing) * scale -
		characterSpacing * scale;
	const int panelWidth = textWidth + paddingX * 2;
	const int panelHeight = glyphHeight * scale + paddingY * 2;
	const int panelLeft = (kMapWidth - panelWidth) / 2;
	const int panelTop = kMapHeight - panelHeight;

	for (int y = panelTop; y < kMapHeight; ++y) {
		for (int x = panelLeft; x < panelLeft + panelWidth; ++x) {
			const std::size_t pixelIndex =
				(static_cast<std::size_t>(y) * kMapWidth + x) * 4;
			pixels[pixelIndex] = 32;
			pixels[pixelIndex + 1] = 32;
			pixels[pixelIndex + 2] = 32;
			pixels[pixelIndex + 3] = 255;
		}
	}

	const int textLeft = panelLeft + paddingX;
	const int textTop = panelTop + paddingY;
	for (std::size_t characterIndex = 0; characterIndex < timestamp.size(); ++characterIndex) {
		const auto glyph = timestampGlyph(timestamp[characterIndex]);
		const int glyphLeft =
			textLeft + static_cast<int>(characterIndex) * (glyphWidth + characterSpacing) * scale;
		for (int row = 0; row < glyphHeight; ++row) {
			for (int column = 0; column < glyphWidth; ++column) {
				if ((glyph[static_cast<std::size_t>(row)] & (1 << (glyphWidth - column - 1))) == 0) {
					continue;
				}
				for (int dy = 0; dy < scale; ++dy) {
					for (int dx = 0; dx < scale; ++dx) {
						const int x = glyphLeft + column * scale + dx;
						const int y = textTop + row * scale + dy;
						const std::size_t pixelIndex =
							(static_cast<std::size_t>(y) * kMapWidth + x) * 4;
						pixels[pixelIndex] = 255;
						pixels[pixelIndex + 1] = 255;
						pixels[pixelIndex + 2] = 255;
						pixels[pixelIndex + 3] = 255;
					}
				}
			}
		}
	}
}

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

bool bindUdpSocket(int port, int& socketFd) {
	socketFd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socketFd < 0) {
		std::cerr << "Could not create UDP socket for port " << port << ": "
				  << std::strerror(errno) << '\n';
		return false;
	}

	const int reuseAddress = 1;
	if (setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress,
				   sizeof(reuseAddress)) < 0) {
		std::cerr << "Could not configure UDP socket for port " << port << ": "
				  << std::strerror(errno) << '\n';
		close(socketFd);
		socketFd = -1;
		return false;
	}

	sockaddr_in localAddress{};
	localAddress.sin_family = AF_INET;
	localAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	localAddress.sin_port = htons(static_cast<uint16_t>(port));
	if (bind(socketFd, reinterpret_cast<const sockaddr*>(&localAddress),
			 sizeof(localAddress)) < 0) {
		std::cerr << "Could not bind UDP port " << port << ": " << std::strerror(errno)
				  << '\n';
		close(socketFd);
		socketFd = -1;
		return false;
	}
	return true;
}

bool waitForDatagram(int socketFd, std::atomic_bool& running, std::atomic_bool& failed) {
	pollfd descriptor{socketFd, POLLIN, 0};
	const int pollResult = poll(&descriptor, 1, kPollTimeoutMilliseconds);
	if (pollResult == 0) {
		return false;
	}
	if (pollResult < 0) {
		if (errno == EINTR) {
			return false;
		}
		std::cerr << currentTimestamp() << " poll failed: " << std::strerror(errno) << '\n';
		failed = true;
		running = false;
		return false;
	}
	if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
		std::cerr << currentTimestamp() << " UDP socket reported a polling error\n";
		failed = true;
		running = false;
		return false;
	}
	return (descriptor.revents & POLLIN) != 0;
}
}  // namespace

VideoGenerator::VideoGenerator(int pointsPort, int mapPort,
							   std::filesystem::path tilesDirectory,
							   std::filesystem::path outputPath)
	: pointsPort_(pointsPort),
	  mapPort_(mapPort),
	  tilesDirectory_(std::move(tilesDirectory)),
	  outputPath_(std::move(outputPath)) {}

bool VideoGenerator::run(std::atomic_bool& running) {
	int pointsSocket = -1;
	int mapSocket = -1;
	if (!bindUdpSocket(pointsPort_, pointsSocket)) {
		return false;
	}
	if (!bindUdpSocket(mapPort_, mapSocket)) {
		close(pointsSocket);
		return false;
	}

	std::cout << "Listening for UDP point batches on port " << pointsPort_ << '\n'
			  << "Listening for map updates (zoom,latitude,longitude) on port " << mapPort_
			  << '\n';
	std::thread pointReceiver(&VideoGenerator::receivePoints, this, pointsSocket,
							  std::ref(running));
	std::thread mapReceiver(&VideoGenerator::receiveMapView, this, mapSocket,
							std::ref(running));
	std::thread mapRenderer(&VideoGenerator::renderMaps, this, std::ref(running));

	while (running) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	stateChanged_.notify_all();

	pointReceiver.join();
	mapReceiver.join();
	mapRenderer.join();
	close(pointsSocket);
	close(mapSocket);
	return !failed_;
}

void VideoGenerator::receivePoints(int socketFd, std::atomic_bool& running) {
	std::array<char, 65536> buffer{};
	std::unordered_map<std::size_t, Transmission> transmissions;

	while (running) {
		if (!waitForDatagram(socketFd, running, failed_)) {
			continue;
		}

		sockaddr_in sender{};
		socklen_t senderLength = sizeof(sender);
		const ssize_t bytesReceived = recvfrom(socketFd, buffer.data(), buffer.size(), 0,
											  reinterpret_cast<sockaddr*>(&sender), &senderLength);
		if (bytesReceived < 0) {
			if (errno == EINTR) {
				continue;
			}
			std::cerr << currentTimestamp() << " recvfrom on point port failed: "
					  << std::strerror(errno) << '\n';
			failed_ = true;
			running = false;
			stateChanged_.notify_all();
			break;
		}

		char senderAddress[INET_ADDRSTRLEN]{};
		if (inet_ntop(AF_INET, &sender.sin_addr, senderAddress, sizeof(senderAddress)) == nullptr) {
			std::strncpy(senderAddress, "unknown", sizeof(senderAddress) - 1);
		}

		const std::string payload(buffer.data(), static_cast<std::size_t>(bytesReceived));
		std::istringstream packet(payload);
		std::string header;
		std::size_t batchId = 0;
		std::size_t totalPoints = 0;
		std::size_t pointOffset = 0;
		if (!std::getline(packet, header) ||
			!parseHeader(header, batchId, totalPoints, pointOffset) || totalPoints == 0 ||
			totalPoints > kMaximumRowsPerTransmission || pointOffset >= totalPoints) {
			std::cerr << currentTimestamp() << " Ignoring malformed point datagram from "
					  << senderAddress << ':' << ntohs(sender.sin_port) << '\n';
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

void VideoGenerator::receiveMapView(int socketFd, std::atomic_bool& running) {
	std::array<char, 65536> buffer{};
	while (running) {
		if (!waitForDatagram(socketFd, running, failed_)) {
			continue;
		}

		const ssize_t bytesReceived =
			recvfrom(socketFd, buffer.data(), buffer.size(), 0, nullptr, nullptr);
		if (bytesReceived < 0) {
			if (errno == EINTR) {
				continue;
			}
			std::cerr << currentTimestamp() << " recvfrom on map port failed: "
					  << std::strerror(errno) << '\n';
			failed_ = true;
			running = false;
			stateChanged_.notify_all();
			break;
		}

		const std::string payload(buffer.data(), static_cast<std::size_t>(bytesReceived));
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
	stateChanged_.notify_one();
}

void VideoGenerator::publishMapView(const MapView& view) {
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		mapView_ = view;
		++revision_;
	}
	stateChanged_.notify_one();
}

void VideoGenerator::renderMaps(std::atomic_bool& running) {
	std::size_t renderedRevision = std::numeric_limits<std::size_t>::max();
	while (running) {
		std::vector<Point> points;
		MapView view;
		std::size_t currentRevision = 0;
		{
			std::unique_lock<std::mutex> lock(stateMutex_);
			stateChanged_.wait(lock, [&] { return !running || revision_ != renderedRevision; });
			if (!running) {
				break;
			}
			points = points_;
			view = mapView_;
			currentRevision = revision_;
		}

		if (renderMap(points, view)) {
			renderedRevision = currentRevision;
			std::cout << currentTimestamp() << " Wrote " << points.size()
					  << " points to PNG " << outputPath_ << '\n';
		} else {
			failed_ = true;
			running = false;
			stateChanged_.notify_all();
			break;
		}
	}
}

bool VideoGenerator::renderMap(const std::vector<Point>& points, const MapView& view) const {
	const PixelCoordinate center = project(view.latitude, view.longitude, view.zoom);
	const double originX = center.x - kMapWidth / 2.0;
	const double originY = center.y - kMapHeight / 2.0;
	std::vector<png_byte> pixels(
		static_cast<std::size_t>(kMapWidth) * kMapHeight * PNG_IMAGE_SAMPLE_CHANNELS(PNG_FORMAT_RGBA),
		0xe6);
	for (std::size_t index = 3; index < pixels.size(); index += 4) {
		pixels[index] = 0xff;
	}

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
			png_image tileImage{};
			tileImage.version = PNG_IMAGE_VERSION;
			if (!png_image_begin_read_from_file(&tileImage, tilePath.c_str())) {
				std::cerr << currentTimestamp() << " Could not decode map tile " << tilePath
						  << ": " << tileImage.message << '\n';
				return false;
			}
			tileImage.format = PNG_FORMAT_RGBA;
			std::vector<png_byte> tilePixels(PNG_IMAGE_SIZE(tileImage));
			if (!png_image_finish_read(&tileImage, nullptr, tilePixels.data(), 0, nullptr)) {
				std::cerr << currentTimestamp() << " Could not read map tile " << tilePath
						  << ": " << tileImage.message << '\n';
				png_image_free(&tileImage);
				return false;
			}
			if (tileImage.width != kTileSize || tileImage.height != kTileSize) {
				std::cerr << currentTimestamp() << " Unexpected dimensions for map tile "
						  << tilePath << ": " << tileImage.width << 'x' << tileImage.height
						  << '\n';
				png_image_free(&tileImage);
				return false;
			}

			const double tileLeft = tileX * tileSize - originX;
			const double tileTop = tileY * tileSize - originY;
			const int startX = std::max(0, static_cast<int>(std::ceil(tileLeft)));
			const int endX = std::min(kMapWidth,
									  static_cast<int>(std::ceil(tileLeft + tileSize)));
			const int startY = std::max(0, static_cast<int>(std::ceil(tileTop)));
			const int endY = std::min(kMapHeight,
									  static_cast<int>(std::ceil(tileTop + tileSize)));
			for (int y = startY; y < endY; ++y) {
				const int sourceY = std::clamp(
					static_cast<int>((y + 0.5 - tileTop) / tileScale), 0, kTileSize - 1);
				for (int x = startX; x < endX; ++x) {
					const int sourceX = std::clamp(
						static_cast<int>((x + 0.5 - tileLeft) / tileScale), 0, kTileSize - 1);
					const std::size_t sourceIndex =
						(static_cast<std::size_t>(sourceY) * kTileSize + sourceX) * 4;
					const std::size_t destinationIndex =
						(static_cast<std::size_t>(y) * kMapWidth + x) * 4;
					const unsigned int alpha = tilePixels[sourceIndex + 3];
					for (std::size_t channel = 0; channel < 3; ++channel) {
						pixels[destinationIndex + channel] = static_cast<png_byte>(
							(tilePixels[sourceIndex + channel] * alpha +
							 pixels[destinationIndex + channel] * (255 - alpha)) /
							255);
					}
				}
			}
			png_image_free(&tileImage);
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
		for (int offsetY = -6; offsetY <= 6; ++offsetY) {
			for (int offsetX = -6; offsetX <= 6; ++offsetX) {
				const int x = centerX + offsetX;
				const int y = centerY + offsetY;
				if (x < 0 || x >= kMapWidth || y < 0 || y >= kMapHeight) {
					continue;
				}
				const int distanceSquared = offsetX * offsetX + offsetY * offsetY;
				if (distanceSquared > 36) {
					continue;
				}
				const std::size_t pixelIndex =
					(static_cast<std::size_t>(y) * kMapWidth + x) * 4;
				const bool outline = distanceSquared >= 20;
				pixels[pixelIndex] = outline ? 255 : 229;
				pixels[pixelIndex + 1] = outline ? 255 : 57;
				pixels[pixelIndex + 2] = outline ? 255 : 53;
				pixels[pixelIndex + 3] = 255;
			}
		}
	}
	drawTimestamp(pixels, currentTimestamp());

	png_image outputImage{};
	outputImage.version = PNG_IMAGE_VERSION;
	outputImage.width = kMapWidth;
	outputImage.height = kMapHeight;
	outputImage.format = PNG_FORMAT_RGBA;
	if (!png_image_write_to_file(&outputImage, temporaryPath.c_str(), 0,
								 pixels.data(), 0, nullptr)) {
		std::cerr << currentTimestamp() << " Failed writing PNG map " << temporaryPath
				  << ": " << outputImage.message << '\n';
		png_image_free(&outputImage);
		removeTemporaryOutput();
		return false;
	}
	png_image_free(&outputImage);

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
