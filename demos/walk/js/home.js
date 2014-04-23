/*
Script: APNG.js
	Animated PNGs, background-image or src based. If background-image is used, background-position transitions are supported. Native APNG fallback for browsers that support it included.

	License:
		MIT-style license.

	Authors:
		Guillermo Rauch
*/

var APNG = new Class({

	Implements: [Options, Class.Occlude, Events],

	options: {
		property: 'src',
		axis: 'x',
		ext: '.png',
		frames: 5,
		endless: true,
		interval: 100,
		autoStart: true,
		startFrame: 1,
		preload: true,
		useNative: Browser.Engine.gecko19 || Browser.Engine.presto950,
		addFilter: Browser.Engine.trident3
	},

	initialize: function(element, options) {
		this.setOptions(options);
		this.element = $(element);
		if (this.occlude('apng')) return this.occluded;
		this.original = this.options.property == 'src' ? this.element.src : this.element.getStyle('background-image').replace(/url\((.*)\)/i, '$1');
		this.basename = this.original.substr(0, this.original.length - this.options.ext.length);
		if (this.options.useNative) {
			this.start = this.reset = this.pause = this.resume = this.cancel = $empty;
		} else {
			if (this.options.preload) this.preload();
			this.reset(!this.options.autoStart);
		}
	},

	setSrc: function(src, index) {
		if (this.options.property != 'background-position') {
			this.options.property == 'src' ? this.element.set('src', src) : this.element.setStyle('background-image', 'url(' + src + ')');
			if (this.options.addFilter) {
				this.element.style.filter = "progid:DXImageTransform.Microsoft.AlphaImageLoader(src='" + src + "',sizingMethod='crop')";
				this.setSrc(APNG.blankImage || '/blank.gif');
			}
		} else {
			var w = this.element['get' + (this.options.axis == 'x' ? 'Width' : 'Height')]();
			px = (index * w) - w;
			this.element.setStyle('background-position', (this.options.axis == 'x' ? -px + 'px 0' : '0 ' + px + 'px'));
		}
	},

	setFrame: function(index, pause) {
		if (!this.stoped) {
			this.current = index;
			var ext = this.options.ext;
			this.setSrc(index == 1 ? this.original : (this.basename + '-' + index + this.options.ext), index);
			if (!pause) this.start();
		}
	},

	preload: function() {
		if (!this.preloaded) {
			this.preloaded = true;
		}
	},

	start: function(anim) {
		var no_delay = false;
		if (anim) {
			this.current_anim = anim;
			this.current = anim[0].start;
			no_delay = true;
			this.stoped = false;
			this.current_anim_part = 0;
			this.current_anim_repeat = 0;
			this.current_anim_length = anim.length;
		}
		this.running = true;
		var delay = no_delay ? 0 : $type(this.options.interval) == 'array' ? this.options.interval[this.current - 1] : this.options.interval;
		var next = true;

		if (this.current_anim) {
			//End of anim part
			if (this.current_anim[this.current_anim_part] && this.current == this.current_anim[this.current_anim_part].end) {
				//Repeat?
				if (this.current_anim_repeat == this.current_anim[this.current_anim_part].repeat) { //No more repeat
					delay += this.current_anim[this.current_anim_part].delay;
					this.current_anim_part++;
					this.current_anim_repeat = 0;
					if (!this.current_anim[this.current_anim_part]) { //End of anim
						next = false;
					} else { //set this.current for next part
						this.current = this.current_anim[this.current_anim_part].start - 1;
					}
				} else { //Repeat again
					this.current_anim_repeat++;
					this.current = this.current_anim[this.current_anim_part].start - 1;
				}
			}
		}
		this.last_timer = this.timer;
		this.timer = (function(timer) {
			$clear(this.last_timer);
			if (next) {
				this.setFrame(this.current + 1);
			} else {
				(this.options.endless ? this.reset() : this.pause());
			}
		}).delay(delay, this);
	},

	reset: function(pause) {
		this.current_anim_part = 0;
		if (this.running) this.pause();
		this.setFrame(this.options.startFrame, pause);
	},
	reset_anim: function() {
		this.current_anim = null;
		this.current_anim_part = 0;
		this.current_anim_length = 0;
		this.current_anim_repeat = 0;
		this.current = 0;
		this.stoped = true;
	},
	stop: function(pause) {
		this.pause();
		this.reset_anim();
	},
	pause: function() {
		$clear(this.timer);
		$clear(this.last_timer);
		this.running = false;
		if (!this.options.endless) {
			this.reset_anim();
			this.fireEvent('complete');
		}
	},

	resume: function() {
		if (!this.running) this.start();
	},

	cancel: function() {
		this.pause();
		this.reset(true);
	}

});
var ape_client, ape_home;
APE.Home = new Class({

	Extends: APE.Client,

	initialize: function() {
		this.fps = 50;
		this.max_txt = 60;
		this.ape = null;
		this.max_user = 20;
		this.coef = 250;
		this.pipe = null;

		this.els = {};
		this.msn_user = {
			ape: $('first_tree')
		};
		this.twitter_user = {
			ape: $('second_tree')
		};
		this.els.container = $('frontdemo');
		this.els.container_top = new Element('div').inject(this.els.container, 'before');
		this.els.container_pos = this.els.container.getCoordinates();
		(function() {
			this.els.container_pos = this.els.container.getCoordinates();
		}.bind(this)).delay(5);

		window.addEvent('resize', function() {
			this.els.container_pos = this.els.container.getCoordinates();
		}.bind(this));

		this.addEvent('load', function() {
			this.core.start({
				"home": true,
				"name": this.rand_chars()
			})
		});
		this.onRaw('twitter', this.raw_twitter);
		this.onRaw('butterfly', this.raw_butterfly);
		this.onRaw('data', function(data, pipe) {
			this.write_message(data.data.msg, data.data.from);
		});
		this.addEvent('init', function() {
			loading_el.destroy()
		});
		this.onRaw('positions', this.raw_positions);
		this.onCmd('send', this.cmd_send);
		this.onCmd('setpos', this.save_user_pos);
		this.onError('004', this.reset);
		this.addEvent('userLeft', this.remove_ape);
		this.addEvent('multiPipeCreate', function(pipe) {
			if (pipe.name.contains('demohome')) {
				this.pipe = pipe;
				this.add_chat();
				this.els.container.addEvent('click', this.set_coord.bindWithEvent(this));
			} else if (pipe.name == '*twitter') {
				this.twitter = pipe;
			}
		});
		this.addEvent('userJoin', this.create_ape);
	},
	rand: function(min, max) {
		return Math.floor(Math.random() * (max - min + 1)) + min;
	},
	show_tweet: function(tweet) {
		var txt = decodeURIComponent(tweet.text);
		var els = this.write_message(txt, this.twitter_user, true, {
			'no_remove': true,
			'style': 'msg_twitter'
		});
		var w = els.cnt.getScrollSize().x;
		els.el.setStyle('margin-left', -w);

		(function() {
			if (Browser.Engine.trident) {
				new Fx.Morph(els.real_cnt, {
					duration: 2000
				}).start({
					'opacity': 0
				});
			}
			var mtop = this.rand(30, 90);
			new Fx.Morph(els.el, {
				transition: Fx.Transitions.linear,
				duration: 2000
			}).start({
				'left': -this.rand(500, 1000),
				'top': (Browser.Engine.trident && !document.querySelectorAll) ? mtop - 30 : mtop,
				'opacity': Browser.Engine.trident ? 1 : 0
			}).addEvent('complete', function() {
				this.destroy();
			}.bind(els.el));
		}.delay(200, this));
	},
	save_user_pos: function(data, pipe) {
		pipe.getUser(this.user.pubid).properties.x = data.x;
	},
	save_pipe: function(pipe) {
		pipe.sessions = {};
	},
	cmd_send: function(cmd, pipe) {
		this.write_message(cmd.msg, this.user);
	},
	raw_twitter: function(res) {
		this.show_tweet.delay($random(100, 600), this, res.data);
	},
	raw_butterfly: function(res) {
		this.write_message(decodeURIComponent(res.data.user + ' : ' + res.data.text), this.msn_user, false, {
			'style': 'msg_msn'
		});
	},
	write_message: function(message, sender, no_hide, options) {
		if (!options) options = {};
		if (!no_hide) no_hide = false;
		//Define sender
		sender = this.pipe.getUser(sender.pubid) || sender;
		var left = sender.ape.getCoordinates().left;

		//destroy old message
		if (!options.no_remove) sender.ape.getElements('.ape_message_container').destroy();

		//Create message container
		var msg = new Element('div', {
			'class': 'ape_message_container ' + options.style
		});
		if (Browser.Engine.trident && !document.querySelectorAll) msg.setStyle('margin-top', 0);
		var left = new Element('div', {
			'class': 'msg_left'
		}).inject(msg);
		var cnt = new Element('div', {
			'class': 'msg'
		}).inject(left);
		message = decodeURIComponent(message);
		var right = new Element('div', {
			'class': 'msg_right',
			'text': message.substring(0, this.max_txt)
		}).inject(cnt);

		//Inject message
		msg.inject(sender.ape);
		if (message.toLowerCase().contains('houba')) {
			this.set_houba(sender);
		}

		//Show message
		new Fx.Morph(cnt, {
			'duration': 200
		}).start({
			'opacity': 1
		});

		//Delete old message
		if (!no_hide) {
			(function(el) {
				$(el).morph({
					'opacity': '0'
				});
				(function() {
					$(this).destroy();
				}).delay(300, el);
			}).delay(5000, this, msg);
		}
		return {
			'el': msg,
			'cnt': right,
			'real_cnt': cnt,
			'left': left
		};
	},
	limit_text: function(ev) {
		var input = this.els.input;
		if (input.get('value').length > this.max_txt) {
			input.set('value', input.get('value').substring(0, this.max_txt));
		}
	},
	add_chat: function() {
		var form = null;

		/*
		//Twitter API changed. This doesn't work anymore :(
		this.msn_talk = new Element('div', {
			'styles': {
				'opacity': 0
			},
			'html': msn_talk,
			'class': 'msn_talk'
		}).inject(this.msn_user.ape);

		this.msn_user.ape.addEvent('mouseover', function() {
			this.msn_talk.morph({
				'opacity': 1
			});
		}.bind(this));
		(function() {
			this.write_message(msn_msg, this.msn_user, false, {
				'style': 'msg_msn'
			});
		}.delay(1500, this));
		this.msn_user.ape.addEvent('mouseout', function() {
			this.msn_talk.morph({
				'opacity': 0
			});
		}.bind(this));*/

		this.els.chat_box_home = new Element('div', {
			'class': 'chat_box_home'
		}).inject($('master_container'), 'before').morph({
			height: 16
		});
		//this.els.chat_box_home.store('fx',new Fx.Morph(this.els.chat_box_home));
		form = new Element('form').inject(this.els.chat_box_home);

		this.els.input = new Element('input', {
			'type': 'text',
			'value': txt_input,
			'class': 'text autoclear'
		}).inject(form);
		add_autoclear(this.els.input);

		this.els.input.addEvent('keydown', this.limit_text.bindWithEvent(this));
		this.els.input.addEvent('keyup', this.limit_text.bindWithEvent(this));
		new Element('input', {
			'type': 'submit',
			'class': 'submit',
			'value': ''
		}).inject(form);

		form.addEvent('submit', function(ev) {
			ev.stop();
			var val = this.els.input.get('value');
			if (val.trim() != '') {
				this.pipe.send(val);
				this.els.input.set('value', '');
			}
		}.bind(this));
	},
	raw_positions: function(raw, pipe) {
		var user = pipe.getUser(raw.data.from.pubid);
		if (raw.data.x) {
			this.move_ape(user, raw.data.x, 0);
		}
	},
	set_coord: function(ev, user) {
		if (!user) user = this.user;
		var new_pos = this.coord_to_relative(ev.page.x) - 35;
		this.move_ape(user, new_pos);
		this.pipe.request.send('SETPOS', {
			'x': new_pos,
			'y': 0
		});
	},
	create_ape: function(user, pipe) {
		if (pipe.name.contains('demohome')) {
			user.ape_container = new Element('div', {
				'class': 'ape_container',
				'styles': {
					'opacity': 0
				}
			});
			user.ape = new Element('div', {
				'class': 'ape'
			}).inject(user.ape_container);
			user.ape_anim = new APNG(user.ape, {
				autoStart: false,
				useNative: false,
				frames: 20,
				endless: true,
				property: 'background-position',
				axis: 'x',
				interval: [1500, 200, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100]
			});

			user.ape_anim.addEvent('complete', function(user) {
				this.set_idle(user);
			}.bind(this, user));
			user.tie = new Element('div', {
				'styles': {
					'background-color': 'rgb(' + this.user_color(user.properties.name) + ')'
				},
				'class': 'ape_tie'
			}).inject(user.ape_container);
			var fx = new Fx.Morph(user.ape_container, {
				duration: 4000,
				transition: Fx.Transitions.linear
			});
			user.ape_container.store('fx', fx);
			fx.addEvent('complete', this.set_idle.bind(this, user));
			this.set_ape_pos(user);
			this.set_idle(user);
			user.ape_container.inject('frontdemo', 'top').morph({
				'opacity': 1
			});
			if (user.pubid == this.core.getPubid()) { //It's Meeeeeeee
				this.user = user;
				this.start_text();
				if (!this.core.options.restore) {
					(function() {
						this.set_coord({
							'page': {
								'x': this.rand(150, 780) + this.els.container_pos.left
							}
						});
					}.delay(200, this));
				}
			}
		}
	},
	go_left: function(user) {
		this.reset_user(user);
		user.ape_container.addClass('go_left');
		user.ape_anim.start([{
			'start': 5,
			'end': 12,
			'delay': 0
		}]);
	},
	go_right: function(user) {
		this.reset_user(user);
		user.ape_container.addClass('go_right');
		user.ape_anim.start([{
			'start': 13,
			'end': 20,
			'delay': 0
		}]);
	},
	rand: function(min, max) {
		return Math.floor(Math.random() * (max - min + 1)) + min;
	},
	reset_user: function(user) {
		user.ape_container.removeClass('go_left');
		user.ape_container.removeClass('go_right');
		user.ape_anim.setFrame(1);
		user.ape_anim.stop();
	},
	set_houba: function(user) {
		this.reset_user(user);
		user.ape_anim.options.endless = false;
		user.ape_anim.start([{
			'start': 3,
			'end': 4,
			'delay': 0,
			'repeat': 1
		}, {
			'start': 1,
			'end': 1,
			'repeat': 0,
			'delay': 0
		}, ]);
	},
	set_idle: function(user) {
		user.ape_anim.options.endless = true;
		this.reset_user(user);
		user.ape_anim.start([{
			'start': 1,
			'end': 1,
			'repeat': 0,
			'delay': this.rand(0, 1000)
		}, {
			'start': 1,
			'end': 2,
			'delay': 0,
			'repeat': this.rand(3, 5)
		}, {
			'start': 1,
			'end': 1,
			'delay': 2500,
			'repeat': 0
		}, {
			'start': 3,
			'end': 4,
			'delay': 0,
			'repeat': 1
		}, {
			'start': 1,
			'end': 1,
			'repeat': 0,
			'delay': 3000
		}]);
	},
	next_link: function(el) {
		new Element('a', {
			'text': msg_next
		}).inject(el.cnt);
		el.el.addEvent('mousedown', function(ev) {
			ev.stop();
			this.next_text();
		}.bind(this));
	},
	start_text: function() {
		if (!this.text_i && !this.core.options.restore) {
			this.text_i = 0;
			if (this.pipe.users.getLength() > this.max_user) {
				this.text = ape_talk;
			} else {
				this.text = ape_talk_alone;
			}
			this.next_text();
		}
	},
	next_text: function() {
		var no_hide = true;
		if (this.text_i == this.text.length - 1) {
			no_hide = false;
		}
		var els = this.write_message(this.text[this.text_i], this.user, no_hide);
		if (no_hide) {
			this.next_link(els);
		}
		this.text_i++;
	},
	move_ape: function(user, x) {
		//Get some info
		var ape = user.ape_container,
			old_pos = this.coord_to_relative(ape.getCoordinates().left),
			new_pos = this.check_pos(x),
			fx = ape.retrieve('fx');

		//Stop event
		fx.cancel();

		//Get way
		if (new_pos < old_pos) { //left
			this.go_left(user);
		} else { //right
			this.go_right(user);
		}
		//Set time
		fx.options.duration = (Math.abs(new_pos - old_pos) / this.fps) * this.coef;

		//Moooooveee
		fx.start({
			'margin-left': new_pos
		});

	},
	user_color: function(nickname) {
		var color = new Array(0, 0, 0);
		var i = 0;
		while (i < 3 && i < nickname.length) {
			//Transformation du code ascii du caractère en code couleur
			color[i] = Math.abs(Math.round(((nickname.charCodeAt(i) - 97) / 26) * 200 + 10));
			i++;
		}
		return color.join(',');
	},
	check_pos: function(x) {
		if (!x) return 63;
		else if (x < 63) return 63;
		else if (x > 790) return 790;
		return x;
	},
	set_ape_pos: function(user) {
		user.ape_container.setStyle('margin-left', this.check_pos(user.properties.x) + 'px');
	},
	coord_to_relative: function(x) {
		return x - this.els.container_pos.left;
	},
	rand_chars: function() {
		var keylist = "abcdefghijklmnopqrstuvwxyz"
		var temp = ''
		var plength = 5;
		for (i = 0; i < plength; i++) {
			temp += keylist.charAt(Math.floor(Math.random() * keylist.length))
		}
		return temp;
	},
	reset: function() {
		this.core.clearSession();
		this.els.container.getElements('.ape_container').destroy();
		if (this.els.chat_box_home) this.els.chat_box_home.destroy();
		//if(this.els.msn_help) this.els.msn_help.destroy();
		this.els.container.removeEvents('click');
		this.core.initialize(this.core.options);
	},
	remove_ape: function(user, pipe) {
		user.ape_container.destroy();
	}
});
var loading_el;
var weeTips = new Class({

	els: {},
	options: {},

	initialize: function(selector, options) {
		this.els.tip = Element('div', {
			'styles': {
				'opacity': '0.8',
				'z-index': 999,
				'position': 'absolute'
			},
			'class': 'tip-wrap'
		}).inject(document.body);
		this.options.offset = {
			'x': 0,
			'y': 0
		}
		var tips = $$(selector);
		tips.each(function(el) {
			el.store('title', el.get('title'));
			el.removeAttribute('title');
		});
		this.els.tip.addEvent('mouseenter', function(ev) {
			$(ev.target).setStyle('display', 'none');
		});
		tips.addEvents({
			'mouseenter': function(ev) {
				var el = $(ev.target);
				this.els.tip.inject(el);
				var text = el.retrieve('title');
				this.els.tip.set('text', text);
				var pos = el.getPosition();
				this.els.tip.setStyle('display', 'block');
				var size = this.els.tip.getSize();
				var elx = (pos.x);
				var ely = (pos.y + 20);

				this.els.tip.setStyles({
					'left': elx,
					'top': ely
				});
			}.bind(this),
			'mouseleave': function() {
				this.els.tip.setStyle('display', 'none');
			}.bind(this)
		});
	}
});
window.addEvent('domready', function() {
	loading_el = new Element('div', {
		'text': 'Your APE is coming...',
		'id': 'home_loading'
	}).inject('frontdemo');
	new weeTips('#home_bullets li');
});


var autoClearValues = new Array();
function add_autoclear(el) {
	autoClearValues[el.get('id')] = el.get('value');
	el.addEvent('focus', function(ev) {
		var el = $(ev.target);
		if (el.get('value') == autoClearValues[el.get('id')]) {
			el.set('value', '');
		}
	});
	el.addEvent('blur', function(ev) {
		var el = $(ev.target);
		if (el.get('value') == '') {
			el.set('value', autoClearValues[el.get('id')]);
		}
	});
}