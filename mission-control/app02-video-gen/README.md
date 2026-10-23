# App 02: Video generator

The receiver listens for point batches and map-view updates on separate UDP ports.
It writes `output.png`, a 1920-by-1080 offline map composited from PNG tiles in
the configured local tile directory. The PNG is refreshed when a point batch is
completed or a map-view packet is received, and an initial map is rendered at
startup. Each generated image includes its local generation timestamp at the
bottom in `YYYY-MM-DD HH:MM:SS.mmm` format.

```sh
./app02_video_gen <points-port> <map-port> <tiles-directory> <output.png>
```

The point port retains the batch format used by `app01-sensor`: each datagram
starts with `batch-id,total-points,point-offset`, followed by one
`latitude,longitude,altitude` row per line. The map port accepts one
`zoom,latitude,longitude` triple per datagram, for example
`7,19.076090,72.877426`. Its default view is zoom 7, centered at
19.076090° N, 72.877426° E. Map packets may specify zoom levels 0 through 7.
The bundled local tiles cover zoom levels 0 through 2; provide matching local
tiles for higher zoom levels to render map imagery at those zooms. When a
requested zoom is unavailable locally, the closest available lower-zoom tiles
are scaled to fit the requested view.

To send a zoom-2 map view to a receiver listening on UDP port 5002:

```sh
printf '2,19.076090,72.877426' | nc -u -w1 127.0.0.1 5002
```
