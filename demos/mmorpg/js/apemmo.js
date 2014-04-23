/*
 * APE MMO DEMO
 *
 * TODO: Synchronize timers.
 *
 *
 *
 *
 */
APE.MmoClient = new Class({
	Extends: APE.Client,
	Implements: Options,
	skills: {
		'fire': {
			cooldown: 1000,
			id: 1,
			incant: 0
		},
		'thunder': {
			cooldown: 2000,
			id: 2,
			incant: 2000
		}
	},
	options: {
		ape: {
			identifier: 'apemmo'
		},
		map: {
			width: 1000,
			height: 1000
		},
		width: 800,
		height: 400,
		listener: document,
		pipe: 'apemmo',
		start: {
			x: 400-16,
			y: 200-48
		},
		pas: 2
	},
	els: {},
	chat: {
		visible: false
	},
	bar: {

	},
	started: false,
	units: new Hash(),
	spells: new Hash(),
	ctx: null,//Canvas 2D Context
	cnt: 0,
	mapimg: null,
	loaded: false,
	perso: false,
	spellCnt: 0,
	drawing: false,
	messages: 0,
	nick: false,
	noReapeatKeys: {
		'up': {},
		'left':{},
		'right':{},
		'down':{}
	},
	keys: {
		'up': {},
		'left':{},
		'right':{},
		'down':{},
		'f2':{},
		'f3':{},
		'enter':{},
		'tab':{}
	},
	keysAlias: {
		'z':'up',
		'w':'up',
		'a':'left',
		'q':'left',
		's':'down',
		'd':'right',
		'1':'f2',
		'2':'f3'
	},
	initialize: function(target){

		this.els.target = target;

		// INIT //
		this.x = this.options.start.x;
		this.y = this.options.start.y;

		Ge.preload('/demos/mmorpg/img/skill1.png');
		Ge.preload('/demos/mmorpg/img/skill2.png');
		Ge.preload('/demos/mmorpg/img/map.png', this.drawDesign.bind(this));

		//## RAWS ##//
		this.addEvent('ready', this.ready);
		this.addEvent('userJoin', this.userJoin);
		this.addEvent('userLeft', this.userLeft);
		this.addEvent('multiPipeCreate', this.multiPipeCreate);
		this.onError('005', this.requestNick);
		this.onError('006', this.requestNick);

		this.onRaw('mmo_start', this.rawStart);
		this.onRaw('mmo_stop', this.rawStop);
		this.onRaw('mmo_firespell', this.rawFireSpell);
		this.onRaw('mmo_incant', this.rawIncant);
		this.onRaw('mmo_player_kill', this.rawPlayerKill);
		this.onRaw('mmo_error', this.rawError);
		this.onRaw('mmo_creep', this.rawCreep);
		this.onRaw('mmo_creeps', this.rawCreeps);
		this.onRaw('mmo_creep_walk', this.rawCreepWalk);
		this.onRaw('mmo_creep_kill', this.rawCreepKill);
		this.onRaw('data', this.rawData);

		//this.onRaw('data', this.rawData);

		//## KEYS ##//
		this.options.listener.addEvent('keydown', this.keyRealDown.bind(this));
		this.options.listener.addEvent('keypress', this.keyPress.bind(this));
		this.options.listener.addEvent('keyup', this.keyRealUp.bind(this));
		
		window.addEvent('domready', function(){
			this.load({
				'identifier': this.options.ape.identifier,
				'complete': this.complete.bind(this)
			});
		}.bind(this));
	},
	requestNick: function(){
		var loop = 0;
		
		if(this.nick !== false) this.els.ptels.span.set('text', 'Please choose another nickname: ');
		
		this.els.prompt.fade('in');
	},
	nickClick: function(){
		this.nick = this.els.ptels.input.get('value');
		if(this.nick){
			this.core.start({'name':this.nick});
		}else{
			this.requestNick();
		}
	},
	complete: function(){
		this.drawDesign();
		this.requestNick();

	},
	ready: function(){

			// START THE TIMER //

			this.interval = setInterval(this.tick.bind(this), 20);

			// JOIN THE CHANNEL //

			this.core.join(this.options.pipe);
		
	},
	drawDesign: function(){
		if(!this.loaded){
			this.loaded = true;
		}else{
			//this.els.container = new Element('div', {'id':'apemmo'});
			
			this.els.container = $('apemmo');

			this.els.canvas = $('canvas');
			this.els.prompt = $('prompt');
			
			this.els.ptels = {};
			this.els.ptels.p = new Element('p', {'class':'ape_name_prompt'});
			this.els.ptels.span = new Element('span', {'text':'Please choose a nickname:'});
			this.els.ptels.input = new Element('input', {
				type: 'text',
				'class':'text'
			});
			this.els.ptels.btn = new Element('button', {
				'text':'Connect',
				'class':'submit'
			});
			this.els.prompt.grab(this.els.ptels.p);
			this.els.ptels.p.adopt(this.els.ptels.span, this.els.ptels.input, this.els.ptels.btn);
			
			this.els.ptels.btn.addEvent('click', this.nickClick.bind(this));
			/*
			this.els.canvas = new Element('canvas', {
				width: this.options.width,
				height: this.options.height
			});
			*/
			this.els.canvas.addEvent('click', this.canvasClick.bind(this));
			this.ctx = this.els.canvas.getContext('2d');

			this.els.info = new Element('div', {'class':'info'});

			// CHAT //

			this.chat.main = new Element('div',{
				'class': 'chat'
			});
			this.chat.msgs = new Element('div',{
				'class':'messages'
			});
			this.chat.input = new Element('input',{
				type:'text'
			});
			this.chat.zone = new Element('div',{
				'style':'display:none'
			})
			this.chat.zone.grab(this.chat.input);
			this.chat.main.adopt(this.chat.msgs, this.chat.zone);

			// THE BAR //

			this.bar.main = new Element('div', {
				'class': 'bar'
			});
			this.bar.spells = new Array();
			for(var i in this.skills){
				var el = new Element('div', {
					'class':'spell',
					style:'background-image:url(\'/demos/mmorpg/img/icon_'+this.skills[i].id+'.png\')'
				});
				var overlay = new Element('div', {'class':'overlay'});
				el.grab(overlay);
				this.bar.spells.push(el);
				this.bar.main.grab(el);

				el.addEvent('click', this.spellClick.bind(this, i));
			}

			// GRAB ALL IN MAIN //

			this.els.container.adopt(this.chat.main, this.els.canvas, this.els.info, this.bar.main);

			$(this.els.target).grab(this.els.container);

			this.redraw();
		}
	},
	multiPipeCreate: function(pipe, data){
		this.pipe = pipe;
		//this.core.request.cycledStack.setTime(1000);
	},
	userJoin: function(user, pipe){
		
		if(user.pubid == this.core.user.pubid){
			this.els.prompt.fade('out');
			this.started = true;
			this.perso = this.addUnit(
				'/demos/mmorpg/img/0'+(user.properties.mmo_avatar)+'.png',
				user.pubid,
				null,
				null,
				user.properties['mmo_life']
			);

		}else if(user.properties['mmo_life'] > 0){
			var x = null
			var y = null;
			if(user.properties && user.properties['posx']){
				x = Number(user.properties['posx']);
				y = Number(user.properties['posy']);
			}
			this.addUnit('/demos/mmorpg/img/0'+(user.properties.mmo_avatar)+'.png', user.pubid, x, y,user.properties['mmo_life']);
		}
		
	},
	userLeft: function(user, pipe){
		this.removeUnit(user.pubid);
	},
	removeUnit: function(pubid){
		if(this.selected == pubid){
			this.selected = false;
			this.perso.stopIncant();
		}
		this.units.erase(pubid);
		if(pubid == this.core.user.pubid){
			this.options.listener.removeEvents('keydown');
			this.options.listener.removeEvents('keypress');
			this.options.listener.removeEvents('keyup');
			this.error('You have been killed, reload this page to re-spawn...', 15000);
		}
	},
	addUnit: function(src, key, x, y, life){

		x = x == null ? this.options.start.x : x;
		y = y == null ? this.options.start.y : y;

		var unit = new Ge.Unit(
			src,
			this.els.canvas,
			{startPosX:x,startPosY:y,width:32,height:48,life:life}
		);
		unit.key = key;
		unit.addEvent('click', this.unitClick.bind(this));

		this.units.set(key, unit);
		return unit;
	},

	// LOG //
	info: function(txt, clas,delay){
		delay = delay || 1000;
		var info = new Element('div', {
			text: txt,
			'class': clas
		});
		this.els.info.grab(info);

		var f = function(el){
			el.fade('out');
			el.destroy.delay(delay, el);
		}
		f.delay(5000, this, info);
	},
	error: function(txt, delay){
		this.info(txt, 'error', delay);
	},

	// NETWORK //

	send: function(cmd, params, addpos){
		if(this.pipe){
			if(addpos){
				params.pos = {
					x: this.x,
					y: this.y
				}
			}
			//console.log('Sending', this.pipe, cmd, params);
			this.pipe.request.send(cmd, params);
		}else{
			//console.log('Sending without pipe');
		}
	},
	sendStart: function(){
		this.send('mmo_start',
			{ dir: this.perso.dir },
			true
		);
	},
	rawStart: function(params, pipe){
		if(params.data.user.pubid == this.core.user.pubid) return;

		var unit = this.units.get(params.data.user.pubid);
		unit.x = params.data.pos.x;
		unit.y = params.data.pos.y;
		unit.rotate(params.data.dir);
		unit.play();
	},
	rawStop: function(params, pipe){
		if(params.data.user.pubid == this.core.user.pubid) return;

		var unit = this.units.get(params.data.user.pubid);
		unit.x = params.data.pos.x;
		unit.y = params.data.pos.y;
		unit.stop();
	},
	rawUpdate: function(params, pipe){
		if(params.data.user.pubid == this.core.user.pubid) return;

		var unit = this.units.get(params.data.user.pubid);
		unit.x = params.data.pos.x;
		unit.y = params.data.pos.y;
		//unit.rotate(params.data.dir);
		//unit.play();
	},
	rawFireSpell: function(params, pipe){
		if(params.data.from == this.core.user.pubid){
			this.perso.stopIncant();
			this.info('You inflicted '+params.data.power+' damages !')
		}else{
			var from = this.units.get(params.data.from);
			if(from) from.stopIncant();
		}
		this.spellOn(params.data.spell, params.data.target, params.data.power);
	},
	rawIncant: function(params, pipe){
		var from_pubid = params.data.from.pubid;

		if(from_pubid == this.core.user.pubid) return;

		var from = this.units.get(from_pubid);
		if(from)
			from.startIncant();
	},
	rawError: function(params, pipe){
		if(params.data.stopIncant) this.perso.stopIncant();
		if(params.data.stop) this.perso.stop();
		if(!params.data.dontShow){
			this.error(params.data.msg);
		}
	},
	rawData: function(params, pipe){
		this.addMessage(params.data.from.properties.name, unescape(params.data.msg));
	},
	rawCreeps: function(params, pipe){
		for (var i in params.data.creeps){
			if(params.data.creeps.hasOwnProperty(i)){
				var creep = params.data.creeps[i];
				var unit = this.addCreep(creep);
			}
		}
	},
	rawCreep: function(params, pipe){
		this.addCreep(params.data.creep);
	},
	rawCreepWalk: function(params, pipe){
		var creep = this.units.get('creep_'+params.data.creep);
		
		if(creep) creep.walkTo(params.data.target.x, params.data.target.y);
	},
	rawCreepKill: function(params, pipe){
		//console.log('kreep id dead');
		var creep = this.units.get('creep_'+params.data.creep);

		if(creep){
			creep.stop();
			creep.extras = false;
			creep.options.loops = 1;
			creep.startAnim();
			//TODO
			creep.addEvent('animationEnd', this.removeUnit.bind(this, ['creep_'+params.data.creep]));
			//console.log('Creep Started death animation', creep);
		}else{
			//console.log('Unknow creep to kill creep_'+params.data.creep);
		}
	},
	rawPlayerKill: function(params, pipe){
		var unit = this.units.get(params.data.target);
		if(unit){
			unit.options.loops = 1;
			unit.extras = false;
			unit.addEvent('animationEnd', this.removeUnit.bind(this, [params.data.target]));
			unit.startAnim(2);
		}
	},
	addCreep: function(creep){
		var unit = this.addUnit(
			'/demos/mmorpg/img/creep'+creep.type+'.png',
			'creep_'+creep.id,
			creep.pos.x,
			creep.pos.y,
			creep.totalLife
		);
		unit.life = creep.life;
		if(creep.target) unit.walkTo(creep.target.x, creep.target.y);
		return unit;
	},
	// KEYS //

	canvasClick: function(ev){
		var pos = this.els.canvas.getCoordinates();
		var x = ev.page.x - pos.left, y = ev.page.y - pos.top;

		var map = this.mapPos();

		this.units.each(function(unit, key){

			var ux, uy;

			if(key != this.core.user.pubid){
				ux = unit.x+map.x, uy = unit.y+map.y;
				if(x > ux && x < ux + 32 && y > uy && y < uy + 48){

				unit.fireEvent('click', {target:unit,ev:ev});
				}
			}
		}.bind(this));

	},

	unitClick: function(ev){
		this.selectUnit(ev.target);
	},
	selectUnit: function(unit){
		if(this.selected){
			var target = this.units.get(this.selected);
			target.unselect();
			this.perso.stopIncant();
		}
		if(unit.key == this.selected) this.selected = false;
		else{
			unit.select();
			this.selected = unit.key;
		}
	},
	keyPress: function(ev){
		if(!this.started) return;
		
		var key = ev.key;
		if(!this.keys[key] && (this.chat.visible || !this.keysAlias[key])) return;
		
		if(this.keysAlias[key]) key = this.keysAlias[key];
		ev.stop();
	},

	keyRealDown: function(ev){
		if(!this.started){
			if(ev.key == 'enter'){
				this.nickClick();
			}
			return;
		}
		
		var key = ev.key;
		if(!this.keys[key] && (this.chat.visible || !this.keysAlias[key])) return;
		if(this.keysAlias[key]) key = this.keysAlias[key];

		ev.stop();

		if(this.noReapeatKeys[key] && this.keys[key].down){
			this.noReapeatKeys[key].uped = 0;
		}else{
			this.keyDown(ev);
		}

	},
	keyRealUp: function(ev){
		if(!this.started) return;
		
		var key = ev.key;
		if(!this.keys[key] && (this.chat.visible || !this.keysAlias[key])) return;
		if(this.keysAlias[key]) key = this.keysAlias[key];
		
		ev.stop();

		if(this.noReapeatKeys[key]){
			this.noReapeatKeys[key].uped = new Date().getTime();
		}else{
			this.keyUp(ev);
		}

	},
	keyDown: function(ev){
		var key = ev.key;
		if(!this.keys[key] && (this.chat.visible || !this.keysAlias[key])) return;

		if(this.keysAlias[key]) key = this.keysAlias[key];
		
		if(key == 'tab'){
			var keys = this.units.getKeys();
			if(!this.selected && keys.length > 1){
				this.selectUnit(this.units.get(keys[1]));
			}else if(keys.length > 1){
				for(var i=0;i<keys.length;i++){
					if(keys[i] == this.selected){
						i++;
						break;
					}
				}
				if(i >= keys.length) i = 0;
				if(keys[i]==this.core.user.pubid) i++;
				if(i >= keys.length) i = 0;
				this.selectUnit(this.units.get(keys[i]));

			}
		}else if(key == 'enter'){
			if(this.chat.visible){
				var val = this.chat.input.get('value')
				if(val != ''){
					this.pipe.send(val);
					this.addMessage(this.nick, val);
					this.chat.input.set('value','');
				}else{
					this.chat.visible = false;
					this.chat.zone.setStyle('display', 'none');
				}
			}else{
				this.chat.visible = true;
				this.chat.zone.setStyle('display', 'block');
				this.chat.input.focus();
			}
		}else if(key=='f2' || key=='f3'){
			if(!this.selected) return this.error('Please select a target');

			this.perso.stopIncant();
			this.perso.stop();

			switch(key){
			    default:
				case 'f2':
					this.spell('fire');
					break;
				case 'f3':
					this.spell('thunder');
					break;
			}
		}else if(!this.keys[key].down && this.perso && this.perso.rotate(key, 0)){
			this.keys[key].down = new Date().getTime();
			this.perso.play();

			this.sendStart();
		}
	},
	keyUp: function(ev){
		var key = ev.key;
		if(!this.keys[key] && (this.chat.visible || !this.keysAlias[key])) return;

		if(this.keysAlias[key]) key = this.keysAlias[key];

		this.keys[key].down = false;
		if(key == this.perso.dir){
			var newdir = '';
			var max = 0;
			for(var i in this.keys){
				if(this.keys[i].down > max){
					max = this.keys[i].down;
					newdir = i;
				}
			}
			if(newdir != ''){
				this.perso.rotate(newdir);
				this.sendStart();
			}else{
				this.perso.stop();
				this.send('mmo_stop', {}, true);
			}
		}
	},
	addMessage: function(nick, msg){

		if(++this.messages > 11){
			this.chat.msgs.getElement('div').destroy();
		}

		var line = new Element('div',{});
		var nickel = new Element('span', {
			'class':'nick',
			'text': nick
		});
		var ptel = new Element('span', {text:':','class':'pt'});
		var msgel = new Element('span', {
			'class':'txt',
			'text':msg
		});
		line.adopt(nickel, ptel, msgel);
		this.chat.msgs.grab(line);
	},

	// SPELL //
	spellClick: function(spell){
		this.spell(spell);
	},
	spell: function(spell){
		if(!this.selected){
			this.error('Target lost');
			return;
		}
		var now = new Date().getTime();

		if(this.skills[spell].last && now - this.skills[spell].last < this.skills[spell].cooldown){
			this.error('This spell is not ready yet !');
		}else{
			this.skills[spell].last = now;
			if(this.skills[spell].incant > 0) this.perso.startIncant();
			this.send('mmo_spell', {'spell':spell, 'target':this.selected});
		}
	},
	spellOn: function(spell, target, power){

		if(target == this.core.user.pubid){
			this.spellAt(spell, this.x, this.y);
			this.perso.life -= power;
			this.info('You lost '+power+' life points !');
		}else{
			var unit = this.units.get(target);

			if(unit){
				var x = unit.x;
				var y = unit.y;
				this.spellAt(spell, x ,y);
				unit.life -= power;
			}else{
				//console.log('Unknow unit',target);
			}
		}
	},
	spellAt: function(type, x, y, onme){
		//console.log('SpellAt', arguments);

		var spell = new Ge.Spell(type, x, y, this.els.canvas, onme);

		this.spells.set(++this.spellCnt, spell);

		spell.anim.play();
		spell.anim.addEvent('animationEnd', this.removeSpell.bind(this, this.spellCnt));

	},
	removeSpell: function(spell){
		this.spells.erase(spell);
	},
	// TIMER //
	tick: function(){
		var now = new Date().getTime();


		if(!this.last_tick) this.last_tick = now - 20;

		while(now - this.last_tick > 10){
			this.last_tick += 20;

			$each(this.noReapeatKeys , function(key, code){
				if(key.uped > 0){
					if(this.last_tick - key.uped > 50){
						this.keyUp({key:code});
						key.uped = 0;
					}
				}
			}.bind(this));

			//console.log('Différence', now - this.last_tick);

			if(this.perso.playing && this.perso.anim == 0){
				var add = this.perso.parseDir(this.perso.dir);
				this.x += add.x*this.options.pas;
				this.y += add.y*this.options.pas;

				this.x = Math.max(this.x, 0);
				this.y = Math.max(this.y, -24);
				this.x = Math.min(this.x, this.options.map.width-8);
				this.y = Math.min(this.y, this.options.map.height-32);
			}

			//if(this.cnt%50==0){ this.send('mmo_update', {}, true); }
			this.redraw(this.cnt++%9==0);

			if(this.cnt%5==0){
				this.spells.each(function(spell, key){
					spell.tick();
				});
			}
		}
	},
	mapPos: function(){
		var mx = this.options.start.x-this.x;
		var my = this.options.start.y-this.y;

		var rx = Math.min(mx, 0);
		var ry = Math.min(my, 0);
		rx = Math.max(rx, this.options.width - this.options.map.width -24);
		ry = Math.max(ry, this.options.height - this.options.map.height -24);

		return {x:rx, y:ry, ox: mx, oy: my};
	},
	redraw: function(tick){


		if(!this.drawing){
			this.drawing = true;
			//// Calc ////
			var map = this.mapPos();

			delete this.drawList;
			this.drawList = new Array();

			// UNITS //
			this.units.each(function(unit, pubid){
				if(tick) unit.tick();

				if(pubid == this.core.user.pubid){

					this.addToDrawList(unit, map.x -map.ox, map.y -map.oy, -map.oy+176);
				}else{
					unit.walk(this.options.pas, this.options.map.width, this.options.map.height);
					this.addToDrawList(unit, map.x, map.y, unit.y+24)
				}
			}.bind(this));

			// SPELLS //
			this.spells.each(function(spell){
				this.addToDrawList(spell, map.x, map.y, spell.z);
			}.bind(this));

			this.ctx.clearRect(0,0,this.options.width, this.options.height);

			this.ctx.drawImage(Ge.getPreloaded('/demos/mmorpg/img/map.png').img, Math.round(map.x), Math.round(map.y));

			this.drawList.each(function(line, z){
				line.each(function(item){
					item[0].draw(item[1], item[2]);
				});
			});
			this.drawing = false;
		}else{
			this.units.each(function(unit, pubid){
				if(tick) unit.tick();
				unit.walk(this.options.pas, this.options.map.width, this.options.map.height);
			}.bind(this));
		}
	},
	addToDrawList: function(item, x, y, z){
		if(!this.drawList[z]) this.drawList[z] = new Array();

		if(item.z != z){
			item.z = z;
			//console.log('Drawing ', item, 'at', [x,y], 'on', z);
		}

		this.drawList[z].push([item,x,y]);
	}
});
