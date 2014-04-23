var Ge = {
	version: 0.1,
	randomLowerCaseLetter: function()
	{
		return String.fromCharCode(97 + Math.round(Math.random() * 25));
	},
	randomWord: function(length){
		var word = '';
		for(var i = 0; i < length; i ++) word += Ge.randomLowerCaseLetter();
		return word;
	},
	preload: function(src, callback){
		if(!Ge.preloaded[src]){
			Ge.preloaded[src] = {};
			if(callback) Ge.preloaded[src].callback = callback;
			Ge.preloaded[src].img = new Image();
			Ge.preloaded[src].img.onload = Ge.imgLoaded.bind(Ge, src);
			Ge.preloaded[src].img.src = src;
		}else if(Ge.preloaded[src].loaded && callback){
			callback(Ge.preloaded[src]);
		}else if(Ge.preloaded[src].callback && callback){
			Ge.preloaded[src].callback = function(img, one, two) {
				one(img);
				two(img);
			}.pass([Ge.preloaded[src], Ge.preloaded[src].callback, callback]);
		}else if(callback){
			Ge.preloaded[src].callback = callback;
		}
	},
	imgLoaded: function(src){
		if(Ge.preloaded[src]){
			Ge.preloaded[src].loaded = true;
			if(Ge.preloaded[src].callback){
				Ge.preloaded[src].callback(Ge.preloaded[src]);
			}
		}
	},
	getPreloaded: function(src, callback){
		Ge.preload(src, callback);
		
		return Ge.preloaded[src];
	},
	preloaded: {}
};



Ge.InCanvasAnimation = new Class({
	version: 0.1,
	Implements: new Array(Options, Events),
	x: 0,
	y: 0,
	ax: 0,
	ay: 0,
	last: 0,
	imgFrames: 0,
	changed: false,
	canvas: {},
	playing: false,
	options: {
		width: 0,
		height: 0,
		startTileX: 0,
		startTileY: 0,
		startPosX: 0,
		startPosY: 0,
		loops: -1,
		drawOnLoad: false
	},
	initialize: function(src, canvas, options){
		this.setOptions(options);

		this.ax = this.options.startTileX;
		this.ay = this.options.startTileY;
		this.x = this.options.startPosX;
		this.y = this.options.startPosY;

		this.canvas.el = canvas;
		this.canvas.ctx = canvas.getContext('2d');
		this.canvas.height = canvas.get('height');
		this.canvas.width = canvas.get('width');

		this.img = Ge.getPreloaded(src,
			function(el){
				this.img = el.img;
				this.loaded = true;
				this.moveToTile(this.ax, this.ay);
				this.imgFrames = Math.floor(this.img.width / this.options.width);
				if(this.options.drawOnLoad) this.draw();
			}.bind(this)
		).img;
		
		this.target = Ge.getPreloaded('/demos/mmorpg/img/target.png',
			function(){ this.target_loaded = true }.bind(this)
		).img;
	},
	move: function(x,y){
		this.x = x;
		this.y = y;
	},
	add: function(x,y){
		this.move(this.x + x, this.y + y);
	},
	moveToTile: function(ax,ay){
		this.ax = ax;
		this.ay = ay;
	},
	draw: function(offsetx, offsety){
		if(this.loaded){
			offsetx = offsetx || 0;
			offsety = offsety || 0;
			var x = this.ax * this.options.width;
			var y = this.ay * this.options.height;
			this.canvas.ctx.drawImage(this.img, x, y, this.options.width,this.options.height, Math.round(this.x+offsetx), Math.round(this.y+offsety), this.options.width,this.options.height);
			this.changed = false;
		}
	},
	play: function(){
		this.playing = true;
	},
	stop: function(){
		this.ax = 0;
		this.playing = false;
	},
	pause: function(){
		this.playing = false;
	},
	tick: function(){
		if(this.playing && this.loaded){
			if(this.ax == this.imgFrames-1 && --this.options.loops == 0){
				this.fireEvent('animationEnd', this);
				this.playing = false;
			}else if(++this.ax == this.imgFrames){
				this.ax = 0;
			}
		}
	}
});
Ge.Spell = new Class({
	z: 0,
	initialize: function(type, x, y, canvas){
		this.z = y + 24;
		switch(type){
			case 'fire':
				this.anim = this.aSpell('/demos/mmorpg/img/skill1.png', x, y, 192, 192, 'center', canvas);
				break;
			default:
			case 'thunder':
				this.anim = this.aSpell('/demos/mmorpg/img/skill2.png', x, y, 192, 192, 'feet', canvas);
				break;
		}
	},
	aSpell: function(src, x, y, w, h, where, canvas){
		switch(where){
			case 'center':
			default:
				x = x + 12 - w / 2;
				y = y + 24 - h / 2;
				break;
			case 'feet':
				x = x + 12 - w / 2;
				y = y + 48 + 24 - h;
				break;
		}
		return new Ge.InCanvasAnimation(src, canvas, {
			startPosX: x,
			startPosY: y,
			width: w,
			height: h,
			loops: 1
		});
	},
	tick: function(){
		this.anim.tick();
	},
	draw: function(offsetx, offsety){
		this.anim.draw(offsetx, offsety);
	}
});


Ge.AnimatedPngs = {
	timer: null
}

Ge.Unit = new Class({
	version: 0.8,
	Extends: Ge.InCanvasAnimation,
	Implements: new Array(Options, Events),
	dirs: {'down':0, 'left':1, 'right':2, 'up':3},
	dir: 'down',
	anim: 0,
	selected: false,
	life: 0,
	extras: true,
	totalLife: 0,
	walking: false,
	targetPos: false,
	initialize: function(img, canvas,opt){
		this.totalLife = opt.life || 0;
		this.life = this.totalLife;
		
		this.parent(img, canvas, opt);
	},
	rotate: function(dir, anim){
		if(anim !== undefined) this.anim = anim;
		dir = dir || this.dir;

		if(!this.idDir(dir)) return false;
		this.dir = dir;

		dir = this.dirs[dir];
		this.ay = dir + this.anim * 4;
		return true;
	},
	parseDir: function(dir){
		var add = {x:0,y:0}
		switch(dir){
			case 'right':
				add.x = 1;
				break;
			case 'left':
				add.x = -1;
				break;
			case 'up':
				add.y = -1;
				break;
			case 'down':
				add.y = 1;
				break;
		}
		return add;
	},
	idDir: function(dir){
		return this.dirs[dir]!=null?true:false;
	},
	stop: function(){
		this.targetPos = false;
		this.parent();
	},
	walkTo: function(x,y){

		this.targetPos = {x:x,y:y};
		this.playing = true;
	},
	walk: function(pas, width, height){
		if(this.targetPos){
			if(this.x < this.targetPos.x){
				this.rotate('right');
				this.x += Math.min(pas/2, this.targetPos.x-this.x);
			}else if(this.x > this.targetPos.x){
				this.rotate('left');
				this.x -= Math.min(pas/2, this.x - this.target.x);
			}else if(this.y < this.targetPos.y){
				this.rotate('down');
				this.y += Math.min(pas/2, this.targetPos.y-this.y);
			}else if(this.y > this.targetPos.y){
				this.rotate('up');
				this.y -= Math.min(pas/2, this.y - this.targetPos.y);
			}else{
				this.targetPos = false;
				this.playing = false;
				this.ax = 0;
			}
		}else if(this.playing && this.anim==0){
			
			var add = this.parseDir(this.dir);

			this.x += add.x * pas;
			this.y += add.y * pas;

			this.x = Math.max(this.x, 0);
			this.y = Math.max(this.y, -24);
			this.x = Math.min(this.x, width-8);
			this.y = Math.min(this.y, height-32);
		}
	},
	draw: function(offsetx, offsety){
		if(this.extras && this.selected && this.target_loaded && this.target){
			offsetx = offsetx || 0;
			offsety = offsety || 0;

			this.canvas.ctx.drawImage(this.target, this.x + offsetx, this.y + offsety + 32);
		}
		
		this.parent(offsetx, offsety);

		if(this.extras && this.totalLife > 0){

			var percent = this.life / this.totalLife * 32;

			this.canvas.ctx.fillStyle = 'rgb(0,255,0)'
			this.canvas.ctx.fillRect(this.x + offsetx, this.y + offsety, percent, 1);

			this.canvas.ctx.fillStyle = 'rgb(255,0,0)'
			this.canvas.ctx.fillRect(this.x + offsetx + percent, this.y + offsety, 32-percent, 1);

		}
	},
	select: function(){
		//TODO
		//this.element.setStyle('background', 'no-repeat bottom center url('+this.options.selectSrc+')');
		this.selected = true;
	},
	unselect: function(){
		//TODO
		//this.element.setStyle('background', 'none');
		this.selected = false;
	},
	startIncant: function(){
		this.incant = true;
		this.startAnim();
	},
	startAnim: function(cnt){
		cnt = cnt || 1;
		if(!this.anim){
			this.anim = cnt;
			this.rotate();
			this.play();
		}
	},
	stopIncant: function(){
		this.incant = false;
		this.stopAnim();
	},
	stopAnim: function(){
		if(this.anim){
			this.anim = 0;
			this.rotate();
			this.stop();
		}
	},
	isunit: true
});