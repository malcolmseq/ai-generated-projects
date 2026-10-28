#include "video_generator.h"

#include <QCoreApplication>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {
void handleSignal(int) {
	if (QCoreApplication::instance()) {
		QCoreApplication::quit();
	}
}

bool parsePort(const char* value, int& port) {
	try {
		std::size_t parsedCharacters = 0;
		const int parsedPort = std::stoi(value, &parsedCharacters);
		if (parsedCharacters != std::string(value).size() || parsedPort < 1 ||
			parsedPort > 65535) {
			return false;
		}
		port = parsedPort;
		return true;
	} catch (const std::exception&) {
		return false;
	}
}
}  // namespace

int main(int argc, char* argv[]) {
	if (argc != 5) {
		std::cerr << "Usage: " << argv[0]
				  << " <points-port> <map-port> <tiles-directory> <output.svg>\n";
		return EXIT_FAILURE;
	}

	int pointsPort = 0;
	int mapPort = 0;
	if (!parsePort(argv[1], pointsPort) || !parsePort(argv[2], mapPort)) {
		std::cerr << "Ports must be integers between 1 and 65535\n";
		return EXIT_FAILURE;
	}
	if (pointsPort == mapPort) {
		std::cerr << "Points and map listeners must use different ports\n";
		return EXIT_FAILURE;
	}

	std::signal(SIGINT, handleSignal);
	std::signal(SIGTERM, handleSignal);

	QCoreApplication application(argc, argv);
	VideoGenerator generator(pointsPort, mapPort, argv[3], argv[4]);
	return generator.run() ? EXIT_SUCCESS : EXIT_FAILURE;
}
