package main

import (
	"osm-cache/internal"

	"github.com/go-chi/chi/v5"
)

func main() {
	osm := internal.Provider{
		Url:         "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
		Dir:         internal.GetTilesPath() + "/osm",
		Attribution: "&copy; <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a> contributors",
	}
	arcgis := internal.Provider{
		Url:         "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
		Dir:         internal.GetTilesPath() + "/arcgis",
		Attribution: "Tiles &copy; Esri &mdash; Source: Esri, i-cubed, USDA, USGS, AEX, GeoEye, Getmapping, Aerogrid, IGN, IGP, UPR-EGP, and the GIS User Community",
	}

	api := internal.HttpApi{
		Router:         chi.NewRouter(),
		BasePath:       internal.GetRootPath(),
		Providers:      map[string]internal.Provider{"osm": osm, "arcgis": arcgis},
		AllowedOrigins: internal.GetAllowedOrigins(),
	}

	api.Run(internal.GetApiPort())
}
