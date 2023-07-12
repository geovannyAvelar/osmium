package main

import (
	"osmium/internal"

	"github.com/go-chi/chi/v5"
)

func main() {
	osm := internal.Provider{
		Url:         "https://tile.openstreetmap.org/{z}/{x}/{y}.png",
		Dir:         internal.GetTilesPath() + "/osm",
		Attribution: "&copy; <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a> contributors",
	}
	arcgis := internal.Provider{
		Url:         "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}.png",
		Dir:         internal.GetTilesPath() + "/arcgis",
		Attribution: "Tiles &copy; Esri &mdash; Source: Esri, i-cubed, USDA, USGS, AEX, GeoEye, Getmapping, Aerogrid, IGN, IGP, UPR-EGP, and the GIS User Community",
	}
	tilezen := internal.Provider{
		Url: "https://tile.nextzen.org/tilezen/terrain/v1/256/terrarium/{z}/{x}/{y}.png",
		Dir: internal.GetTilesPath() + "/tilezen",
	}
	lukla := internal.Provider{
		Url: "http://localhost:9000/64/{z}/{x}/{y}.png",
		Dir: internal.GetTilesPath() + "/lukla",
	}

	api := internal.HttpApi{
		Router:         chi.NewRouter(),
		BasePath:       internal.GetRootPath(),
		Providers:      map[string]internal.Provider{"osm": osm, "arcgis": arcgis, "tilezen": tilezen, "lukla": lukla},
		AllowedOrigins: internal.GetAllowedOrigins(),
	}

	api.Run(internal.GetApiPort())
}
