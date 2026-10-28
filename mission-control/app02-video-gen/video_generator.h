#pragma once

#include <QObject>
#include <QTimer>
#include <QUdpSocket>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <vector>

struct Point {
	double latitude;
	double longitude;
	double altitude;
};

class VideoGenerator : public QObject {
	Q_OBJECT

public:
	VideoGenerator(int pointsPort, int mapPort, std::filesystem::path tilesDirectory,
				   std::filesystem::path outputPath);
	VideoGenerator(const VideoGenerator&) = delete;
	VideoGenerator& operator=(const VideoGenerator&) = delete;

	bool run();

private slots:
	void onPointsReadyRead();
	void onMapReadyRead();
	void renderNextFrame();

private:
	struct MapView {
		int zoom = 7;
		double latitude = 19.076090;
		double longitude = 72.877426;
	};

	bool bindUdpSocket(QUdpSocket& socket, int port);
	void publishPoints(std::vector<Point> points);
	void publishMapView(const MapView& view);
	bool renderMap(const std::vector<Point>& points, const MapView& view) const;

	int pointsPort_;
	int mapPort_;
	std::filesystem::path tilesDirectory_;
	std::filesystem::path outputPath_;
	std::mutex stateMutex_;
	std::vector<Point> points_;
	MapView mapView_;
	std::size_t revision_ = 1;
	std::size_t renderedRevision_ = 0;
	std::atomic_bool failed_{false};
	QUdpSocket pointsSocket_;
	QUdpSocket mapSocket_;
	QTimer renderTimer_;
};
