<?php


	

	function error($line, $die=true){
		$f = fopen('save.log', 'a');
		fwrite($f, $line."\n");
		fclose($f);
		if($die) die($line);
	}
	error(print_r($_POST, true), false);

	function unCompress($ar, $size=6){
		$res = array();
		$cnt = count($ar);

		for($i=0;$i<$cnt;$i++) {

			//if longuer than size

			if($ar[$i][$size]){
				$v = substr($ar[$i], 0, $size);
				$c = (int)substr($ar[$i], $size);
				if(!$cnt) error('ERROR decompressing.');
				while($c-- > 0) {
					array_push($res, $v);
				}
			}else{
				array_push($res, $ar[$i]);
			}
		}
		return $res;
	}


	$list = unCompress(explode(',',$_POST['img']));
	
	//error('Saving : '.print_r($list, true), false);


	if($_POST['pass'] != 'pixelpasswd' || !isset($_POST['img'])) error('ERROR, wrong password or bad param.');

	$width = 640;
	$height = 480;


	$name = date('Y-m-d_H-i-s_'.substr((string)microtime(), 2, 8));

	$image = imagecreatetruecolor($width, $height);
	$s_image = imagecreatetruecolor($width/10, $height/10);


	$cnt = count($list);
	$TRUEcnt = ($width*$height) / 100;

	if($cnt < $TRUEcnt ) {
		error ('Wrong pixels count ('.$cnt.'/'.$TRUEcnt.').');
	}
	for($i=0;$i<$TRUEcnt;$i++) {
		$x = ($i % ($width/10));
		$y = floor($i / ($width/10));

		$c = sscanf($list[$i], '%2x%2x%2x');
		$color = imagecolorallocate($image, $c[0], $c[1], $c[2]);

		imagefilledrectangle($image, $x*10, $y*10, $x*10+10, $y*10+10, $color);
		imagesetpixel($s_image, $x, $y, $color);

		imagecolordeallocate($image, $color);
	}
	imagepng($image, 'saves/big/'.$name.'.png');
	imagepng($s_image, 'saves/small/'.$name.'.png');

	unset ($image);
	unset ($w_image);

	echo $name;
?>
