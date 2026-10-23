#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifndef APP03_SOURCE_DIR
#error "APP03_SOURCE_DIR must be defined by CMake"
#endif

namespace {
constexpr int kDefaultPort = 8080;
constexpr int kDefaultMapUdpPort = 1235;
constexpr int kMinimumZoom = 0;
constexpr int kMaximumZoom = 8;
constexpr int kTileSize = 256;
constexpr double kPanPixels = 256.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaximumMercatorLatitude = 85.0511287798066;
constexpr std::size_t kMaximumRequestSize = 8192;
constexpr std::uintmax_t kMaximumFileSize = 32 * 1024 * 1024;
std::atomic_bool running{true};

struct MapView {
	int zoom = 7;
	double latitude = 19.076090;
	double longitude = 72.877426;
};

struct Route {
	const char* filePath;
	const char* contentType;
};

void handleSignal(int) {
	running = false;
}

bool parsePort(const char* value, int& port) {
	try {
		std::size_t parsedCharacters = 0;
		const int parsedPort = std::stoi(value, &parsedCharacters);
		if (parsedCharacters != std::string(value).size() ||
			parsedPort < 1 || parsedPort > 65535) {
			return false;
		}
		port = parsedPort;
		return true;
	} catch (const std::exception&) {
		return false;
	}
}

bool sendAll(int socketFd, const char* data, std::size_t size) {
	std::size_t sent = 0;
	while (sent < size) {
		const ssize_t result = send(socketFd, data + sent, size - sent, MSG_NOSIGNAL);
		if (result < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (result == 0) {
			return false;
		}
		sent += static_cast<std::size_t>(result);
	}
	return true;
}

bool sendResponse(int socketFd, int status, const std::string& reason,
				  const std::string& contentType, const std::string& body,
				  bool headRequest = false) {
	std::ostringstream headers;
	headers << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
			<< "Content-Type: " << contentType << "\r\n"
			<< "Content-Length: " << body.size() << "\r\n"
			<< "Cache-Control: no-store\r\n"
			<< "Connection: close\r\n\r\n";
	const std::string headerText = headers.str();
	if (!sendAll(socketFd, headerText.data(), headerText.size())) {
		return false;
	}
	return headRequest || sendAll(socketFd, body.data(), body.size());
}

bool sendStatusOnly(int socketFd, int status, const std::string& reason) {
	std::ostringstream headers;
	headers << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
			<< "Content-Length: 0\r\n"
			<< "Cache-Control: no-store\r\n"
			<< "Connection: close\r\n\r\n";
	const std::string headerText = headers.str();
	return sendAll(socketFd, headerText.data(), headerText.size());
}

bool readRequestLine(int socketFd, std::string& requestLine) {
	std::string request;
	request.reserve(1024);
	char buffer[1024];
	while (request.size() < kMaximumRequestSize) {
		const ssize_t received = recv(socketFd, buffer, sizeof(buffer), 0);
		if (received < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (received == 0) {
			return false;
		}
		request.append(buffer, static_cast<std::size_t>(received));
		const std::size_t lineEnd = request.find("\r\n");
		if (lineEnd != std::string::npos) {
			requestLine = request.substr(0, lineEnd);
			return true;
		}
	}
	return false;
}

double wrappedLongitude(double longitude) {
	longitude = std::fmod(longitude + 180.0, 360.0);
	if (longitude < 0.0) {
		longitude += 360.0;
	}
	return longitude - 180.0;
}

bool sendMapView(int udpSocket, const sockaddr_in& destination, const MapView& view) {
	std::ostringstream packet;
	packet << view.zoom << ',' << std::fixed << std::setprecision(6)
		   << view.latitude << ',' << view.longitude;
	const std::string payload = packet.str();
	const ssize_t bytesSent = sendto(udpSocket, payload.data(), payload.size(), 0,
									 reinterpret_cast<const sockaddr*>(&destination),
									 sizeof(destination));
	if (bytesSent < 0 || static_cast<std::size_t>(bytesSent) != payload.size()) {
		std::cerr << "Could not send map view over UDP: " << std::strerror(errno) << '\n';
		return false;
	}
	return true;
}

bool applyControlRoute(const std::string& path, MapView& view) {
	if (path == "/zoom/increment") {
		view.zoom = std::min(kMaximumZoom, view.zoom + 1);
		return true;
	}
	if (path == "/zoom/decrement") {
		view.zoom = std::max(kMinimumZoom, view.zoom - 1);
		return true;
	}

	if (path != "/pan/top" && path != "/pan/bottom" &&
		path != "/pan/right" && path != "/pan/left") {
		return false;
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

	const double worldHeight = worldSize;
	const double clampedCenterY = std::clamp(centerY, 0.0, worldHeight);
	const double mercatorY = kPi * (1.0 - 2.0 * clampedCenterY / worldHeight);
	view.latitude = std::atan(std::sinh(mercatorY)) * 180.0 / kPi;
	view.longitude = wrappedLongitude(centerX / worldSize * 360.0 - 180.0);
	return true;
}

bool findRoute(const std::string& path, Route& route) {
	if (path == "/index" || path == "/index.html") {
		route = {"index.html", "text/html; charset=utf-8"};
		return true;
	}
	if (path == "/controller" || path == "/controller.html") {
		route = {"controller.html", "text/html; charset=utf-8"};
		return true;
	}
	if (path == "/build/test.png") {
		route = {"../app02-video-gen/build/test.png", "image/png"};
		return true;
	}
	return false;
}

void handleClient(int socketFd, int udpSocket, const sockaddr_in& udpDestination,
				  MapView& mapView) {
	std::string requestLine;
	if (!readRequestLine(socketFd, requestLine)) {
		sendResponse(socketFd, 400, "Bad Request", "text/plain; charset=utf-8",
					 "Bad Request\n");
		return;
	}

	std::istringstream request(requestLine);
	std::string method;
	std::string target;
	std::string httpVersion;
	if (!(request >> method >> target >> httpVersion) ||
		(httpVersion != "HTTP/1.0" && httpVersion != "HTTP/1.1")) {
		sendResponse(socketFd, 400, "Bad Request", "text/plain; charset=utf-8",
					 "Bad Request\n");
		return;
	}
	if (method != "GET" && method != "HEAD") {
		sendResponse(socketFd, 405, "Method Not Allowed", "text/plain; charset=utf-8",
					 "Method Not Allowed\n");
		return;
	}

	const std::size_t queryStart = target.find('?');
	const std::string path = target.substr(0, queryStart);
	MapView updatedView = mapView;
	if (applyControlRoute(path, updatedView)) {
		if (method == "HEAD") {
			sendStatusOnly(socketFd, 405, "Method Not Allowed");
			return;
		}
		if (!sendMapView(udpSocket, udpDestination, updatedView)) {
			sendStatusOnly(socketFd, 502, "Bad Gateway");
			return;
		}
		mapView = updatedView;
		sendStatusOnly(socketFd, 200, "OK");
		return;
	}

	Route route{};
	if (!findRoute(path, route)) {
		sendResponse(socketFd, 404, "Not Found", "text/plain; charset=utf-8",
					 "Not Found\n", method == "HEAD");
		return;
	}

	const std::filesystem::path filePath =
		std::filesystem::path(APP03_SOURCE_DIR) / route.filePath;
	std::error_code error;
	const std::uintmax_t fileSize = std::filesystem::file_size(filePath, error);
	if (error) {
		std::cerr << "Could not inspect " << filePath << ": " << error.message() << '\n';
		sendResponse(socketFd, 404, "Not Found", "text/plain; charset=utf-8",
					 "Not Found\n", method == "HEAD");
		return;
	}
	if (fileSize > kMaximumFileSize ||
		fileSize > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
		sendResponse(socketFd, 413, "Content Too Large", "text/plain; charset=utf-8",
					 "Content Too Large\n", method == "HEAD");
		return;
	}

	std::ifstream file(filePath, std::ios::binary);
	if (!file) {
		std::cerr << "Could not open " << filePath << '\n';
		sendResponse(socketFd, 404, "Not Found", "text/plain; charset=utf-8",
					 "Not Found\n", method == "HEAD");
		return;
	}
	std::string body(static_cast<std::size_t>(fileSize), '\0');
	if (!body.empty()) {
		file.read(body.data(), static_cast<std::streamsize>(body.size()));
		if (!file || static_cast<std::size_t>(file.gcount()) != body.size()) {
			std::cerr << "Could not read " << filePath << '\n';
			sendResponse(socketFd, 500, "Internal Server Error",
						 "text/plain; charset=utf-8", "Internal Server Error\n",
						 method == "HEAD");
			return;
		}
	}
	sendResponse(socketFd, 200, "OK", route.contentType, body, method == "HEAD");
}

int runServer(int port, int mapUdpPort) {
	const int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (serverSocket < 0) {
		std::cerr << "Could not create HTTP socket: " << std::strerror(errno) << '\n';
		return EXIT_FAILURE;
	}

	const int reuseAddress = 1;
	if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuseAddress,
				   sizeof(reuseAddress)) < 0) {
		std::cerr << "Could not configure HTTP socket: " << std::strerror(errno) << '\n';
		close(serverSocket);
		return EXIT_FAILURE;
	}

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(static_cast<std::uint16_t>(port));
	if (bind(serverSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
		std::cerr << "Could not bind HTTP port " << port << ": " << std::strerror(errno)
				  << '\n';
		close(serverSocket);
		return EXIT_FAILURE;
	}
	if (listen(serverSocket, 16) < 0) {
		std::cerr << "Could not listen on HTTP port " << port << ": "
				  << std::strerror(errno) << '\n';
		close(serverSocket);
		return EXIT_FAILURE;
	}

	const int udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (udpSocket < 0) {
		std::cerr << "Could not create map UDP socket: " << std::strerror(errno) << '\n';
		close(serverSocket);
		return EXIT_FAILURE;
	}
	sockaddr_in udpDestination{};
	udpDestination.sin_family = AF_INET;
	udpDestination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	udpDestination.sin_port = htons(static_cast<std::uint16_t>(mapUdpPort));
	MapView mapView;

	std::cout << "Serving controller pages at http://localhost:" << port
			  << "/index and http://localhost:" << port << "/controller\n"
			  << "Sending map updates to UDP port " << mapUdpPort << '\n';
	while (running) {
		pollfd serverDescriptor{serverSocket, POLLIN, 0};
		const int pollResult = poll(&serverDescriptor, 1, 500);
		if (pollResult == 0) {
			continue;
		}
		if (pollResult < 0) {
			if (errno == EINTR) {
				continue;
			}
			std::cerr << "Could not poll HTTP socket: " << std::strerror(errno) << '\n';
			close(udpSocket);
			close(serverSocket);
			return EXIT_FAILURE;
		}
		if ((serverDescriptor.revents & POLLIN) == 0) {
			std::cerr << "HTTP socket reported a polling error\n";
			close(udpSocket);
			close(serverSocket);
			return EXIT_FAILURE;
		}

		sockaddr_in clientAddress{};
		socklen_t clientAddressLength = sizeof(clientAddress);
		const int clientSocket =
			accept(serverSocket, reinterpret_cast<sockaddr*>(&clientAddress),
				   &clientAddressLength);
		if (clientSocket < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (!running) {
				break;
			}
			std::cerr << "Could not accept HTTP connection: " << std::strerror(errno) << '\n';
			continue;
		}

		handleClient(clientSocket, udpSocket, udpDestination, mapView);
		close(clientSocket);
	}
	close(udpSocket);
	close(serverSocket);
	return EXIT_SUCCESS;
}
}  // namespace

int main(int argc, char* argv[]) {
	if (argc > 3) {
		std::cerr << "Usage: " << argv[0] << " [http-port] [map-udp-port]\n";
		return EXIT_FAILURE;
	}
	int port = kDefaultPort;
	int mapUdpPort = kDefaultMapUdpPort;
	if ((argc >= 2 && !parsePort(argv[1], port)) ||
		(argc == 3 && !parsePort(argv[2], mapUdpPort))) {
		std::cerr << "Ports must be integers between 1 and 65535\n";
		return EXIT_FAILURE;
	}

	std::signal(SIGINT, handleSignal);
	std::signal(SIGTERM, handleSignal);
	return runServer(port, mapUdpPort);
}