APE.Pixelbox = new Class({
	Extends: APE.Client,
	//Options//
	apeidentifier: 'pixelbox',
	pixelsize: 10,
	ctx: null,
	els: {},
	pickers: {},
	fcolor: new Color([$random(0,200),$random(0,200),$random(0,200)], 'rgb').hex.substr(1),
	bcolor: '000000',
	mouse: {},
	users: new Hash(),
	tools: new Hash(),
	tool: 'pencil',
	lastPoint: 0,
	grid: null,
	points: [],
	oldSaves: null,
	loaded: 0,
	initialize: function(target,canvas, saves){
		window.addEvent('domready', this.domReady.bind(this, [target, canvas]));
		this.oldSaves = saves;
	},
	domReady: function(target, canvas){


		this.els.target = $(target);
		this.els.canvas = $(canvas);

		this.width = this.els.canvas.width / this.pixelsize;
		this.height = this.els.canvas.height / this.pixelsize;

		this.grid = new Array(this.width * this.height);

		this.ctx = this.els.canvas.getContext('2d');

		this.drawDesign();

		this.addEvent('multiPipeCreate', function(pipe){
			this.pipe = pipe;
		}.bind(this));
		this.onRaw('color_change', this.rawColorChange);
		this.onRaw('color', this.rawColor);
		this.onRaw('grid', this.rawGrid);
		this.onRaw('new_pixelbox_save', this.rawNewSave);

		this.addEvent('userJoin', this.userJoin);
		this.addEvent('userLeft', this.userLeft);
		this.addEvent('load', this.loadEv);
		this.addEvent('ready', this.ready);
		
		window.addEvent('keypress', this.keyPress.bind(this));
		
		this.onError('005', this.nickError);//Nick used
		this.onError('006', this.nickError);//Nick invalid

		this.timer = setInterval(this.tick.bind(this), 20);

		for(var i = 0; i < this.oldSaves.length; i++) {
			var save = new APE.Pixelbox.Save(this.oldSaves[i]);
			this.els.gallery.grab(save);
		}
		
		this.load({
			'identifier': this.apeidentifier,
			'channel':['pixelbox']
		});
	},
	userJoin: function(user, channel){
		this.addUser(user.pubid, user.properties.name, user.properties.color);
	},
	userLeft: function(user, channel){
		var usr = this.users.get(user.pubid);
		if(usr) {
			usr.remove();
			this.users.erase(user.pubid);
		}
	},
	addUser: function(key, name, color){
		var user = new APE.Pixelbox.User(name, color);
		this.users.set(key, user);
		this.els.users.grab(user);
	},
	drawDesign: function() {

		// Overlay //
		this.els.overlay = new Element('div', {
			'class':'overlay'
		});
		this.els.popup = {};
		this.els.popup.main = new Element('div', {
			'class':'popup'
		});

		this.els.popup.p = new Element('p', {
			'class':'ape_name_prompt'
		});
		this.els.popup.msg = new Element('span', {
			'text':'Please choose a nickname: '
		});
		this.els.popup.input = new Element('input', {
			'type':'text',
			'name':'nick',
			'autocomplete':'off',
			'class':'text'
		});
		this.els.popup.btn = new Element('button', {
			'text':'Ok',
			'class':'submit'
		});

		this.els.popup.input.addEvent('keydown', function(ev){if(ev.key=='enter'){ this.nickSubmit(ev) }}.bind(this));
		this.els.popup.btn.addEvent('click', this.nickSubmit.bind(this));
		
		this.els.popup.p.adopt(this.els.popup.msg, this.els.popup.input, this.els.popup.btn);
		this.els.popup.main.grab(this.els.popup.p);


		this.els.overlay.grab(this.els.popup.main);

		// Main //
		this.els.canvas.addEvent('mousedown', this.canvasMouse.bind(this));
		this.els.canvas.addEvent('mouseup', this.canvasMouse.bind(this));
		this.els.canvas.addEvent('mousemove', this.canvasMouse.bind(this));

		// target //
		this.els.target.addClass('pixelbox');

		// TOOLS //

		this.els.pencil = new Element('img', {
			'class':'tool',
			'alt':'P',
			'title':'Pencil',
			'src':'/demos/pixelbox/img/pencil.png'
		});
		this.els.pencil.addEvent('click', this.toolClick.bind(this, ['pencil']));
		this.tools.set('pencil', this.els.pencil);

		this.els.pencil = new Element('img', {
			'class':'tool',
			'opacity': 0.5,
			'alt':'E',
			'title':'Eye Dropper',
			'src':'/demos/pixelbox/img/eyedropper.gif'
		});
		this.els.pencil.addEvent('click', this.toolClick.bind(this, ['eyedropper']));
		this.tools.set('eyedropper', this.els.pencil);

		/*this.els.save = new Element('img', {
			'class': 'tool',
			'alt':'Save',
			'title':'Save image to gallery',
			'src':'/demos/pixelbox/img/save.png'
		});
		this.els.save.addEvent('click', function(){
			this.pipe.request.send('save_pixelbox');
		}.bind(this));*/

		// Gallery //
		this.els.gallery = new Element('div', {
			'class':'gallery'
		});

		// Table //

		this.els.tools = new Element('div', {
			'class':'tools'
		});
		this.els.colors = new Element('div', {
			'class':'colors'
		});
		this.els.fcolor = new Element('div', {
			'class':'fcolor',
			'style': 'background-color:  #'+this.fcolor
		});
		this.els.bcolor = new Element('div', {
			'class':'bcolor',
			'style': 'background-color: #'+this.bcolor
		});
		this.els.arrow = new Element('img', {
			'class':'switch',
			'src':'/demos/pixelbox/img/switch.png',
			'alt': 'S'
		});
		this.els.arrow.addEvent('click', this.switchColors.bind(this));
		this.els.colors.adopt(this.els.fcolor, this.els.bcolor, this.els.arrow);

		this.tools.each(function(el){
			this.els.tools.grab(el);
		}.bind(this));


		//this.els.tools.adopt(this.els.save,this.els.colors,this.els.gallery);
		this.els.tools.adopt(this.els.colors,this.els.gallery);

		// USERS //

		this.els.users = new Element('div', {
			'class':'users'
		});


		this.els.target.grab(this.els.tools);
		this.els.target.grab(this.els.users, 'top');
		this.els.target.grab(this.els.overlay, 'top');

		// Colors pickers //
		this.pickers.fcolor = new MooRainbow(this.els.fcolor, {
			'id':'fcolor',
			'onChange': this.moorainbowOnChange.bind(this),
			'onComplete': this.moorainbowOnComplete.bind(this),
			'startColor': [0,0,0]
		});
		this.pickers.bcolor = new MooRainbow(this.els.bcolor, {
			'id':'bcolor',
			'onChange': this.moorainbowOnChange.bind(this),
			'onComplete': this.moorainbowOnComplete.bind(this),
			'startColor': [255,255,255]
		});


		this.els.popup.input.focus();
	},
	nickError: function(params){

		this.els.popup.input.removeProperty('disabled');
		this.els.popup.input.set('value', '');
		this.els.popup.input.focus();

		this.loaded--;

		this.els.popup.msg.set('text', msg);
	},
	loadEv: function(){
		this.incrementJoin();
	},
	nickSubmit: function(){
		if(!this.els.popup.input.get('disabled')) {
			var val = this.els.popup.input.get('value');
			if(!val) return;
			else {
				this.els.popup.input.set('disabled', 'disabled');
				this.nick = val;
				this.incrementJoin();
			}
		}
	},
	incrementJoin: function(){
		if(this.core && this.core.options.restore) {
			this.core.start({}, false);
			this.core.request.stack.add('restore_pixelbox');
			this.core.request.stack.send();
		}else{
			this.loaded++;
			if(this.loaded >= 2) {
				this.core.start({
					'name': this.nick,
					'color': this.fcolor
				});
			}
		}
	},
	ready: function(){//Connected
		this.els.overlay.set('tween', {duration: 'long'});
		this.els.overlay.tween('opacity', 0);

	},
	moorainbowOnChange: function(color, moor) {
		moor.element.setStyle('background-color', color.hex);
		this[moor.options.id] = String(color.hex).substr(1);
	},
	moorainbowOnComplete: function(color, moor) {
		var clr = String(color.hex).substr(1);
		this.setColor(moor.options.id, clr);
	},
	rawNewSave: function(params, raw){
		var save = new APE.Pixelbox.Save(params.data.url+'.png');
		this.els.gallery.grab(save, 'top');
	},
	tick: function(){
		var now = this.now();
		for(var i in this.points) {
			if(this.points.hasOwnProperty(i)) {
				if(this.points[i].when <= now) {
					this.drawPoint(this.points[i].pos, this.points[i].color);
					delete this.points[i];
				}
			}
		}
	},
	rawColor: function(params, raw) {
		var now = this.now();

		var user = this.users.get(params.data.from.pubid);
		if(user) {
			user.last = user.last == 0 || now - user.last > 500 ? now : params.data.delay + user.last;

			//console.log('AddPoint at ',user.last);
			this.points.push({
				when: user.last+100,
				pos: params.data.pos,
				color: params.data.color
			});
		}else{
			this.drawPoint(params.data.pos, params.data.color);
		}
	},
	rawColorChange: function(params, raw) {
		var usr = this.users.get(params.data.from.pubid);
		if(usr){
			usr.setColor(params.data.color);
		}
	},
	rawGrid: function(params, raw) {
		var length = params.data.grid.length;
		for(var i = 0; i < length; i++){
			this.drawPoint(i, params.data.grid[i]);
		}
	},
	setColor: function (type, color){
		if(type=='fcolor') {
			if(this.pipe) this.pipe.request.send('color_change', {color:color});
			this.users.get(this.core.user.pubid).setColor(color);
		}
		this[type] = color;
		this.els[type].setStyle('background-color', '#'+color);
		this.pickers[type].manualSet('#'+color, 'hex');
	},
	switchColors: function(){
		var fc = this.fcolor;
		var bc = this.bcolor;

		this.setColor('fcolor', bc);
		this.setColor('bcolor', fc);
	},
	keyPress: function(ev){
		if(ev.key == 'x'){
			this.switchColors();
		}
	},
	canvasMouse: function(ev) {
		if((this.tool == 'eyedropper' || (ev.alt || ev.control)) && ev.type == 'mousedown'){

			var pos = this.mousePos(ev);

			this.setColor('fcolor', this.grid[this.coorToPos(pos.x, pos.y)]);

			this.toolClick('pencil');

		}else if(this.tool == 'pencil') {
			switch(ev.type){
				case 'mousedown':
					if(ev.rightClick) return;
					var pos = this.mousePos(ev);
					if(ev.shift && this.mouse.pos){
						this.drawLine(this.mouse.pos.x,this.mouse.pos.y,pos.x,pos.y);
					}
					this.mouse.down = true;
					this.mouse.pos = pos;
					this.drawAtMouse();
					break;
				case 'mouseup':
					if(ev.rightClick) return;
					this.mouse.down = false;
					break;
				case 'mousemove':
					if(this.mouse.down){
						var newp = this.mousePos(ev);
						if(this.mouse.pos && !this.comparePoint(newp, this.mouse.pos)){
							this.drawLine(this.mouse.pos.x, this.mouse.pos.y, newp.x, newp.y);
							this.mouse.pos = newp;
							this.drawAtMouse();
						}
					}
					break;
			}
		}
	},
	toolClick: function(tool){
		if(tool != this.tool) {

			var old_tool = this.tools.get(this.tool);
			var new_tool = this.tools.get(tool);
			this.tool = tool;

			old_tool.tween('opacity', 0.5);
			new_tool.tween('opacity', 1);
		}
	},
	comparePoint: function(pt1, pt2){
		return pt1.x == pt2.x && pt1.y == pt2.y;
	},
	mousePos: function(ev){
		var cp = this.els.canvas.getPosition();
		var x = Math.floor((ev.page.x - cp.x - 4)/this.pixelsize);
		var y = Math.floor((ev.page.y - cp.y - 4)/this.pixelsize);

		x = Math.max(x, 0);
		y = Math.max(y, 0);

		x = Math.min(x, this.width);
		y = Math.min(y, this.height);

		return {x:x,y:y};
	},
	drawAtMouse: function(){
		this.sendPixel(this.mouse.pos.x, this.mouse.pos.y, this.fcolor);
	},
	sendPixel: function(x, y, color){
		color = color || this.fcolor;
		if(this.pipe){
			var now = this.now();

			var delay = 0;
			if(this.lastPoint) delay = now - this.lastPoint;
			this.lastPoint = now;

			var pos = this.coorToPos(x,y);
			this.pipe.request.cycledStack.add('color', {pos:pos,color:color,delay:delay});
			this.drawPoint(pos, color);
			//this.pipe.request.send('color', {pos:{x:x,y:y},color:color});
		}
	},
	drawPoint: function(pt, color) {
		var coor = this.posToCoor(pt);
		this.drawPixel(coor.x, coor.y, color);
	},
	drawPixel: function(x, y, color){
		var pos = this.coorToPos(x, y);
		this.grid[pos] = color;

		this.ctx.fillStyle = '#'+color;
		this.ctx.fillRect(x*this.pixelsize, y*this.pixelsize, this.pixelsize, this.pixelsize);
	},
	now: function(){
		var d = new Date();
		return d.getMilliseconds() + d.getSeconds()*1000 + d.getMinutes()*60*1000 + d.getHours()*60*60*1000;
	},
	coorToPos: function(x, y) {
		return y*this.width + x;
	},
	posToCoor: function(pos) {
		return {x:pos%this.width, y:Math.floor(pos/this.width)};
	},
	// Implementation of Bresenham's line algorithm http://en.wikipedia.org/wiki/Bresenham%27s_line_algorithm
	drawLine: function(x1,y1,x2,y2,drawFirst){

		drawFirst = drawFirst?1:0;

		var dx, dy;

		if( (dx = x2 - x1) != 0 ) {
			if( dx > 0 ) {
				if( (dy = y2 - y1 ) != 0 ) {
					if( dy >= 0 ) {// vecteur oblique dans le 1er quadran


						if( dx >= dy ) {
							// vecteur diagonal ou oblique proche de l’horizontale, dans le 1er octant déclarer entier e ;
							var e;
							dx = (e = dx) * 2; dy *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if( ++x1 == x2 ) break;
								if( (e -= dy) < 0 ){
									++y1;
									e += dx;
								}
							}
						} else {
							// vecteur oblique proche de la verticale, dans le 2nd octant
							var e;
							dy = (e = dy) * 2; dx *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if( ++y1 == y2 ) break;
								if( (e -= dx ) < 0 ){
									++x1;
									e += dy;
								}
							}
						}
					} else {// vecteur oblique dans le 4e cadran
						if(dx >= -dy) { // vecteur diagonal ou oblique proche de l’horizontale, dans le 8e octant
							var e;
							dx = (e = dx) * 2; dy *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if( ++x1 == x2 ) break;
								if( (e += dy) < 0 ) {
									--y1;
									e += dx;
								}
							}
						} else { // vecteur oblique proche de la verticale, dans le 7e octant
							var e;
							dy = (e = dy) * 2 ; dx *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if( --y1 == y2 ) break;
								if( (e += dx) > 0) {
									++x1;
									e += dy;
								}
							}
						}
					}
				} else { // vecteur horizontal vers la droite
					do{
						if(drawFirst++) this.sendPixel(x1,y1);
					}while( ++x1 != x2);
				}
			} else { // dx < 0

				if( (dy = y2 - y1) != 0 ) {

					if( dy > 0 ) { // vecteur oblique dans le 2nd quadran
						
						if( -dx >= dy ) { // vecteur diagonal ou oblique proche de l’horizontale, dans le 4e octant
							var e;
							dx = (e = dx) * 2; dy *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if( --x1 == x2 ) break;
								if( (e += dy) >= 0 ) {
									++y1;
									e += dx;
								}
							}
						} else {// vecteur oblique proche de la verticale, dans le 3e octant
							var e;
							dy = (e = dy) * 2; dx *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if( ++y1 == y2) break;
								if( (e += dx ) <= 0) {
									--x1;
									e += dy;
								}
							}
						}
					} else { // vecteur oblique dans le 3e cadran

						if( dx <= dy ) { // vecteur diagonal ou oblique proche de l’horizontale, dans le 5e octant

							var e;
							dx = (e = dx) * 2; dy *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if(--x1 == x2) break;
								if( (e -= dy) >= 0) {
									--y1;
									e += dx;
								}
							}
						} else { // vecteur oblique proche de la verticale, dans le 6e octant
							var e;
							dy = (e = dy) *2; dx *= 2;
							while(true) {
								if(drawFirst++) this.sendPixel(x1,y1);
								if( --y1 == y2) break;
								if( (e -= dx) >= 0) {
									--x1;
									e += dy;
								}
							}
						}
					}
				} else { // vecteur horizontal vers la gauche
					do{
						if(drawFirst++) this.sendPixel(x1,y1);
					}while(--x1 != x2 );
				}
			}
		} else {
			if( (dy = y2 - y1) != 0) {
				if( dy > 0) {
					do{
						if(drawFirst++) this.sendPixel(x1,y1);
					}while( ++y1 != y2 );
				} else {
					do{
						if(drawFirst++) this.sendPixel(x1,y1);
					}while( --y1 != y2);
				}
			}
		}
	}
});
APE.Pixelbox.User = new Class({
	els: {},
	last: 0,
	initialize: function(login, color){
		this.els.name = new Element('span', {
			'class':'nick',
			'text':login
		});
		this.els.color = new Element('span', {
			'class':'color',
			'style':'background-color:#'+color
		});
		//this.els.color.set('tween', {duration:'long'})
		this.els.container = new Element('div', {
			'class':'user'
		});
		this.els.container.adopt(this.els.color, this.els.name);
	},
	toElement: function() {
		return this.els.container;
	},
	setColor: function(clr) {
		this.els.color.tween('background-color', '#'+clr);
	},
	remove: function() {
		this.els.container.get('tween').addEvent('complete', function(el){
			el.destroy();
		});
		this.els.container.tween('height', 0);
	}
});
APE.Pixelbox.Save = new Class({
	els: {},
	name: '',
	initialize: function(name){
		this.name = name;

		this.els.link = new Element('a', {
			'class':'ReMooz',
			'href':'/demos/pixelbox/saves/big/'+this.name
		});
		this.els.image = new Element('img', {
			'class':'save',
			'src': '/demos/pixelbox/saves/small/'+this.name,
			'alt':'Save'
		});
		this.els.link.grab(this.els.image);
		new ReMooz(this.els.link, {
			'centered': true,
			'origin': this.els.image
		});
	},
	toElement: function() {
		return this.els.link;
	}
});
