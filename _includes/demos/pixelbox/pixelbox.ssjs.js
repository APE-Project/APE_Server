include('framework/mootools.js');
include('framework/Http.js');
include('utils/debug.js');

var the_channel = 'pixelbox';
var the_width = 64;
var the_height = 48;
var the_grid = new Array(the_width*the_height);

var save_url = 'http://example.com/pixelsave.php';


for(var i = 0; i < the_width*the_height; i++){
	the_grid[i] = 'ffffff';
}


Ape.addEvent('init', function(){
	//var_export(the_grid);
});
Ape.registerHookCmd("connect", function(params, cmd) {
	if(params.color) {
		cmd.user.setProperty('color', params.color);
	}else{
		return {};
	}
});
Ape.addEvent('beforeJoin', function(user, channel){
	/*
	if(channel.getProperty('name') == the_channel){
		user.setProperty('color', '000');
	}
	*/
});
Ape.addEvent('afterJoin', function(user, channel){
	if(channel.getProperty('name') == the_channel) {
		user.pipe.sendRaw('grid', {grid:the_grid});
	}
});
Ape.registerCmd('restore_pixelbox', true, function(params, infos) {
	infos.user.pipe.sendRaw('grid', {grid:the_grid});
});
Ape.registerCmd('color', true, function(params, infos){
	if(!$defined(params.pos) || !params.color || params.color.length != 6 || params.pos < 0 || params.pos > the_grid.length){
		Ape.log('Bad params color '+params.color+' - '+params.pos );
		return 0;
	}
	the_grid[params.pos] = params.color;
	
	Ape.getPipe(params.pipe).sendRaw('color', {pos:params.pos,color:params.color,delay:params.delay}, {from:infos.user.pipe});
});
Ape.registerCmd('save_pixelbox', true, function(params, infos) {

	var request = new Http(save_url);
	
	request.set('method', 'POST');

	request.writeObject({'pass':'pixelpasswd','img':compress(the_grid) });
	
	request.getContent(imageSaved.bindWithEvent(null, [params.pipe]));
});
function imageSaved(result, pipe){
	Ape.log('Saved :'+result+' - '+pipe);
	if(result != 'ERROR') {
		var channel = Ape.getPipe(pipe).sendRaw('new_pixelbox_save', {url:result});
	}else{
		Ape.log('Error while saving image');
	}
}
function compress(ls){

	var res = new Array();
	var length = ls.length;
	var size = ls[0].length;

	var k = 0;

	for(var i=0;i<length;) {
		Ape.log('looping');

		res[k] = ls[i];
		
		if(i == length - 1) break;
		
		var cnt = 1;

		while(ls[++i] == res[k]){
			//Ape.log('YOOO ' + res[k]);
			if (!$defined(ls[i])) {
				Ape.log('PIXEL SUXXXX');
			}
			cnt++;
		}

		if(cnt > 1) res[k] += String(cnt);
		++k;
	}
	return res.join(',');

}
Ape.registerCmd('color_change', true, function(params, infos){
	if(!params.color){
		Ape.log('Bad params color_change');
		return 0;
	}
	infos.user.setProperty('color', params.color);
	Ape.getPipe(params.pipe).sendRaw('color_change', {'color':params.color}, {from:infos.user.pipe});
});

Ape.log('[JS] PixelBox Started !');
