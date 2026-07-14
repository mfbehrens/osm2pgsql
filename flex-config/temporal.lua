-- This config example file is released into the Public Domain.

-- This is a Lua config for the Flex output demonstrating temporal table support.
-- It requires PostgreSQL 19+ and input from an .osh.pbf file.
--
-- Usage:
--   osm2pgsql --slim -x -O flex -S flex-config/temporal.lua --temporal data.osh.pbf

print('osm2pgsql version: ' .. osm2pgsql.version)

local tables = {}

-- Node table with valid_at for temporal tracking
tables.pois = osm2pgsql.define_node_table('pois', {
    { column = 'tags', type = 'jsonb' },
    { column = 'geom', type = 'point', not_null = true },
    { column = 'valid_at', sql_type = 'tstzrange' },
})

-- Restaurant table with extra columns and valid_at
tables.restaurants = osm2pgsql.define_node_table('restaurants', {
    { column = 'name', type = 'text' },
    { column = 'cuisine', type = 'text' },
    { column = 'geom', type = 'point', not_null = true },
    { column = 'valid_at', sql_type = 'tstzrange' },
})

-- Way table with valid_at
tables.ways = osm2pgsql.define_way_table('ways', {
    { column = 'tags', type = 'jsonb' },
    { column = 'geom', type = 'linestring', not_null = true },
    { column = 'valid_at', sql_type = 'tstzrange' },
})

-- Area/polygon table with valid_at
tables.polygons = osm2pgsql.define_area_table('polygons', {
    { column = 'type', type = 'text' },
    { column = 'tags', type = 'jsonb' },
    { column = 'geom', type = 'geometry', not_null = true },
    { column = 'valid_at', sql_type = 'tstzrange' },
})

for name, dtable in pairs(tables) do
    print("\ntable '" .. name .. "':")
    print("  name='" .. dtable:name() .. "'")
end

function osm2pgsql.process_node(object)
    local valid_at = object:valid_at()

    if object.tags.amenity == 'restaurant' then
        tables.restaurants:insert({
            name = object.tags.name,
            cuisine = object.tags.cuisine,
            geom = object:as_point(),
            valid_at = valid_at,
        })
    else
        tables.pois:insert({
            tags = object.tags,
            geom = object:as_point(),
            valid_at = valid_at,
        })
    end
end

function osm2pgsql.process_way(object)
    local valid_at = object:valid_at()

    if object.is_closed then
        tables.polygons:insert({
            type = object.type,
            tags = object.tags,
            geom = object:as_polygon(),
            valid_at = valid_at,
        })
    else
        tables.ways:insert({
            tags = object.tags,
            geom = object:as_linestring(),
            valid_at = valid_at,
        })
    end
end

function osm2pgsql.process_relation(object)
    local valid_at = object:valid_at()

    if object.tags.type == 'multipolygon' or
       object.tags.type == 'boundary' then
         tables.polygons:insert({
            type = object.type,
            tags = object.tags,
            geom = object:as_multipolygon(),
            valid_at = valid_at,
        })
    end
end
