#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>

namespace {
constexpr double kCenterLatitude = 19.0760;
constexpr double kCenterLongitude = 72.8777;
constexpr double kRadiusKm = 3000.0;
constexpr double kMaximumAltitudeMeters = 500.0;
constexpr std::size_t kMaximumDatagramBytes = 1400;
constexpr std::array<std::size_t, 3> kBatchSizes{50000, 100000, 150000};

struct Point {
	double latitude;
	double longitude;
	double altitude;
};

Point randomPoint(std::mt19937_64& generator) {
	constexpr double kEarthRadiusKm = 6371.0;
	constexpr double kPi = 3.14159265358979323846;
	std::uniform_real_distribution<double> unit(0.0, 1.0);

	const double distanceKm = kRadiusKm * std::sqrt(unit(generator));
	const double bearing = 2.0 * kPi * unit(generator);
	const double latitudeOffset = distanceKm * std::cos(bearing) / kEarthRadiusKm;
	const double longitudeOffset = distanceKm * std::sin(bearing) /
								   (kEarthRadiusKm * std::cos(kCenterLatitude * kPi / 180.0));

	return {
		kCenterLatitude + latitudeOffset * 180.0 / kPi,
		kCenterLongitude + longitudeOffset * 180.0 / kPi,
		unit(generator) * kMaximumAltitudeMeters,
	};
}

bool sendBatch(int socketFd, const sockaddr_in& destination, std::size_t batchId,
			   std::size_t pointCount, std::mt19937_64& generator,
			   std::size_t& totalBytesSent) {
	std::array<char, kMaximumDatagramBytes> datagram{};
	std::size_t pointsSent = 0;
	totalBytesSent = 0;

	while (pointsSent < pointCount) {
		const int headerSize = std::snprintf(datagram.data(), datagram.size(), "%zu,%zu,%zu\n",
											 batchId, pointCount, pointsSent);
		if (headerSize < 0 || static_cast<std::size_t>(headerSize) >= datagram.size()) {
			std::cerr << "Could not format datagram header\n";
			return false;
		}

		std::size_t bytesUsed = static_cast<std::size_t>(headerSize);
		std::size_t pointsInDatagram = 0;
		while (pointsSent + pointsInDatagram < pointCount) {
			const Point point = randomPoint(generator);
			std::array<char, 64> record{};
			const int rowSize = std::snprintf(record.data(), record.size(), "%.6f,%.6f,%.1f\n",
											   point.latitude, point.longitude, point.altitude);
			if (rowSize < 0 || static_cast<std::size_t>(rowSize) >= record.size()) {
				std::cerr << "Could not format point record\n";
				return false;
			}
			if (bytesUsed + static_cast<std::size_t>(rowSize) > datagram.size()) {
				break;
			}
			std::memcpy(datagram.data() + bytesUsed, record.data(), static_cast<std::size_t>(rowSize));
			bytesUsed += static_cast<std::size_t>(rowSize);
			++pointsInDatagram;
		}

		const ssize_t bytesSent = sendto(socketFd, datagram.data(), bytesUsed, 0,
										 reinterpret_cast<const sockaddr*>(&destination),
										 sizeof(destination));
		if (bytesSent < 0 || static_cast<std::size_t>(bytesSent) != bytesUsed) {
			std::cerr << "sendto failed: " << std::strerror(errno) << '\n';
			return false;
		}
		totalBytesSent += static_cast<std::size_t>(bytesSent);
		pointsSent += pointsInDatagram;
	}
	return true;
}
}  // namespace

int main(int argc, char* argv[]) {
	if (argc != 3) {
		std::cerr << "Usage: " << argv[0] << " <destination-ipv4> <port>\n";
		return EXIT_FAILURE;
	}

	char* end = nullptr;
	const long port = std::strtol(argv[2], &end, 10);
	if (end == argv[2] || *end != '\0' || port < 1 || port > 65535) {
		std::cerr << "Port must be an integer between 1 and 65535\n";
		return EXIT_FAILURE;
	}

	sockaddr_in destination{};
	destination.sin_family = AF_INET;
	destination.sin_port = htons(static_cast<uint16_t>(port));
	if (inet_pton(AF_INET, argv[1], &destination.sin_addr) != 1) {
		std::cerr << "Destination must be a valid IPv4 address\n";
		return EXIT_FAILURE;
	}

	const int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socketFd < 0) {
		std::cerr << "Could not create UDP socket: " << std::strerror(errno) << '\n';
		return EXIT_FAILURE;
	}

	std::random_device seed;
	std::mt19937_64 generator(seed());
	std::size_t batchId = 0;
	auto nextBatch = std::chrono::steady_clock::now();

	while (true) {
		std::this_thread::sleep_until(nextBatch);
		const std::size_t pointCount = kBatchSizes[batchId % kBatchSizes.size()];
		std::size_t totalBytesSent = 0;
		if (!sendBatch(socketFd, destination, batchId, pointCount, generator, totalBytesSent)) {
			close(socketFd);
			return EXIT_FAILURE;
		}

		const auto now = std::chrono::system_clock::now();
		const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
			now.time_since_epoch()) % 1000;
		const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
		std::tm localTime{};
		localtime_r(&timestamp, &localTime);

		std::cout << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << '.'
				  << std::setfill('0') << std::setw(3) << milliseconds.count()
				  << std::setfill(' ') << " Sent batch " << batchId << " with "
				  << pointCount << " points, " << totalBytesSent << " bytes\n";
		++batchId;
		nextBatch += std::chrono::seconds(1);
	}
}
