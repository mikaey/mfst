<?php

require_once( 'config.php' ); // Set this to the path to your config.php

$db = new mysqli( MYSQL_HOST, MYSQL_USER, MYSQL_PASS, MYSQL_DBNAME, MYSQL_PORT );
if( $db->connect_error ) {
    header( $_SERVER[ 'SERVER_PROTOCOL' ] . ' 500 Internal Server Error' );
    die( json_encode( [ 'error' => 'Unable to connect to the MySQL server.' ] ) );
}

$query = 'SELECT a.id id, a.name name, a.size size, b.status, b.rate, (b.cur_round_num + b.round_num_offset) cur_round_num, b.num_bad_sectors num_bad_sectors, b.consolidated_sector_map data, b.last_updated last_updated FROM cards a, consolidated_sector_maps b WHERE a.id = b.id AND b.is_active = 1';

if( array_key_exists( 'since', $_REQUEST ) ) {
    $since = intval( $_REQUEST[ 'since' ] );
    $query .= ' AND b.last_updated >= ' . $since;
}

$query .= ' ORDER BY name';

$result = $db->query( $query );

$output = [];

while( $row = $result->fetch_object() ) {
    $output[] = [
        'id' => $row->id,
        'name' => $row->name,
        'data' => base64_encode( $row->data ),
        'last_updated' => $row->last_updated,
        'size' => $row->size,
        'status' => $row->status,
        'rate' => $row->rate,
        'num_bad_sectors' => $row->num_bad_sectors,
        'cur_round_num' => $row->cur_round_num
    ];
}

$result->free();
$db->close();

echo json_encode( $output );

