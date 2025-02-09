\pset tuples_only on
-- Variables for testing
\set resolution 10
\set hexagon '\'8a63a9a99047fff\''
\set meter ST_SetSRID(ST_Point(6196902.235389061,1413172.0833316022), 3857)
\set degree ST_SetSRID(ST_Point(55.6677199224442,12.592131261648213), 4326)

SELECT h3_geo_to_h3(:degree, :resolution) = '8a63a9a99047fff';

-- meters are NOT reprojected
SELECT h3_geo_to_h3(:meter, :resolution) <> '8a63a9a99047fff';

-- check back/forth conversion return same hex
SELECT h3_geo_to_h3(h3_to_geometry(:hexagon), :resolution) = '8a63a9a99047fff';

-- check num points in boundary
SELECT ST_NPoints( h3_to_geo_boundary_geometry(:hexagon)) = 7;

-- test strict h3_geo_to_h3 throws for bad latlon
CREATE FUNCTION h3_test_postgis_nounit() RETURNS boolean LANGUAGE PLPGSQL
    AS $$
        BEGIN
            PERFORM h3_geo_to_h3(POINT(360, 2.592131261648213), 1);
            RETURN false;
        EXCEPTION WHEN OTHERS THEN
            RETURN true;
        END;
    $$;
SET h3.strict TO true;
SELECT h3_test_postgis_nounit();
SET h3.strict TO false;
DROP FUNCTION h3_test_postgis_nounit;

-- Test wraparound
\set lon 55.6677199224442
\set lat 12.592131261648213
SELECT h3_geo_to_h3(POINT(:lon,       :lat), 7)
     = h3_geo_to_h3(POINT(:lon + 360, :lat), 7);

SELECT h3_geo_to_h3(POINT(:lon, :lat      ), 7)
     = h3_geo_to_h3(POINT(:lon, :lat + 360), 7);