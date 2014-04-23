/**
 * ReMooz - Zoomer
 *
 * Inspired by so many boxes and zooms
 *
 * @version		1.0
 *
 * @license		MIT-style license
 * @author		Harald Kirschner <mail [at] digitarald.de>
 * @copyright	Author
 */

var ReMooz = new Class({

	Implements: [Events, Options, Chain],

	options: {
		link: null,
		type: 'image',
		container: null,
		className: null,
		centered: false,
		dragging: true,
		closeOnClick: true,
		shadow: (Browser.Engine.trident) ? 'onOpenEnd' : 'onOpen', // performance
		resize: true,
		margin: 20,
		resizeFactor: 0.95,
		resizeLimit: false, // {x: 640, y: 640}
		fixedSize: false,
		cutOut: true,
		addClick: true,
		opacityLoad: 0.6,
		opacityResize: 1,
		opacityTitle: 0.9,
		resizeOptions: {},
		fxOptions: {},
		closer: true,
		parse: false, // 'rel'
		parseSecure: false,
		temporary: false,
		onBuild: $empty,
		onLoad: $empty,
		onOpen: $empty,
		onOpenEnd: $empty,
		onClose: $empty,
		onCloseEnd: $empty,
		generateTitle: function(el) {
			var text = el.get('title');
			if (!text) return false;
			var title = text.split(' :: ');
			var head = new Element('h6', {'html': title[0]});
			return (title[1]) ? [head, new Element('p', {'html': title[1]})] : head;
		}
	},

	initialize: function(element, options) {
		this.element = $(element);
		this.setOptions(options);
		if (this.options.parse) {
			var obj = this.element.getProperty(this.options.parse);
			if (obj && (obj = JSON.decode(obj, this.options.parseSecure))) this.setOptions(obj);
		}
		var origin = this.options.origin;
		this.origin = ((origin) ? $(origin) || this.element.getElement(origin) : null) || this.element;
		this.link = this.options.link || this.element.get('href') || this.element.get('src');
		this.container = $(this.options.container) || this.element.getDocument();
		this.bound = {
			'click': function(e) {
				this.open.delay(1, this);
				return false;
			}.bind(this),
			'close': this.close.bind(this),
			'dragClose': function(e) {
				if (e.rightClick) return;
				this.close();
			}.bind(this)
		};
		if (this.options.addClick) this.bindToElement();
	},

	destroy: function() {
		if (this.box) this.box.destroy();
		this.box = this.tweens = this.body = this.content = null;
	},

	bindToElement: function(element) {
		($(element) || this.element).addClass('remooz-element').addEvent('click', this.bound.click);
		return this;
	},

	getOriginCoordinates: function() {
		var coords = this.origin.getCoordinates();
		delete coords.right;
		delete coords.bottom;
		return coords;
	},

	open: function(e) {
		if (this.opened) return (e) ? this.close() : this;
		this.opened = this.loading = true;
		if (!this.box) this.build();
		this.coords = this.getOriginCoordinates();
		this.coords.opacity = this.options.opacityLoad;
		this.coords.display = '';
		this.tweens.box.set(this.coords);
		this.box.addClass('remooz-loading');
		ReMooz.open(this.fireEvent('onLoad'));
		this['open' + this.options.type.capitalize()]();
		return this;
	},

	finishOpen: function() {
		this.tweens.fade.start(0, 1);
		this.drag.attach();
		this.fireEvent('onOpenEnd').callChain();
	},

	close: function() {
		if (!this.opened) return this;
		this.opened = false;
		ReMooz.close(this.fireEvent('onClose'));
		if (this.loading) {
			this.box.setStyle('display', 'none');
			return this;
		}
		this.drag.detach();
		this.tweens.fade.cancel().set(0).fireEvent('onComplete');
		if (this.tweens.box.timer) this.tweens.box.clearChain();
		var vars = this.getOriginCoordinates();
		if (this.options.opacityResize != 1) vars.opacity = this.options.opacityResize;
		this.tweens.box.start(vars).chain(this.closeEnd.bind(this));
		return this;
	},

	closeEnd: function() {
		if (this.options.cutOut) this.element.setStyle('visibility', 'visible');
		this.box.setStyle('display', 'none');
		this.fireEvent('onCloseEnd').callChain();
		if (this.options.temporary) this.destroy();
	},

	openImage: function() {
		var tmp = new Image();
		tmp.onload = tmp.onabort = tmp.onerror = function(fast) {
			this.loading = tmp.onload = tmp.onabort = tmp.onerror = null;
			if (!tmp.width || !this.opened) {
				this.fireEvent('onError').close();
				return;
			}
			var to = {x: tmp.width, y: tmp.height};
			if (!this.content) this.content = $(tmp).inject(this.body);
			else tmp = null;
			this[(this.options.resize) ? 'zoomRelativeTo' : 'zoomTo'].create({
				'delay': (tmp && fast !== true) ? 1 : null,
				'arguments': [to],
				'bind': this
			})();
		}.bind(this);
		tmp.src = this.link;
		if (tmp && tmp.complete && tmp.onload) tmp.onload(true);
	},

	/**
	 * @todo Test implementation
	 */
	openElement: function() {
		this.content = this.content || $(this.link) || $E(this.link);
		if (!this.content) {
			this.fireEvent('onError').close();
			return;
		}
		this.content.inject(this.body);
		this.zoomTo({x: this.content.scrollWidth, y: this.content.scrollHeight});
	},

	zoomRelativeTo: function(to) {
		var scale = this.options.resizeLimit;
		if (!scale) {
			scale = this.container.getSize();
			scale.x *= this.options.resizeFactor;
			scale.y *= this.options.resizeFactor;
		}
		for (var i = 2; i--;) {
			if (to.x > scale.x) {
				to.y *= scale.x / to.x;
				to.x = scale.x;
			} else if (to.y > scale.y) {
				to.x *= scale.y / to.y;
				to.y = scale.y;
			}
		}
		return this.zoomTo({x: to.x.toInt(), y: to.y.toInt()});
	},

	zoomTo: function(to) {
		to = this.options.fixedSize || to;
		var box = this.container.getSize(), scroll = this.container.getScroll();
		var pos = (!this.options.centered) ? {
			x: (this.coords.left + (this.coords.width / 2) - to.x / 2).toInt()
				.limit(scroll.x + this.options.margin, scroll.x + box.x - this.options.margin - to.x),
			y: (this.coords.top + (this.coords.height / 2) - to.y / 2).toInt()
				.limit(scroll.y + this.options.margin, scroll.y + box.y - this.options.margin - to.y)
		} :  {
			x: scroll.x + ((box.x - to.x) / 2).toInt(),
			y: scroll.y + ((box.y - to.y) / 2).toInt()
		};
		if (this.options.cutOut) this.element.setStyle('visibility', 'hidden');
		this.box.removeClass('remooz-loading');
		var vars = {left: pos.x, top: pos.y, width: to.x, height: to.y};
		if (this.options.opacityResize != 1) vars.opacity = [this.options.opacityResize, 1];
		else this.box.set('opacity', 1);
		this.tweens.box.start(vars).chain(this.finishOpen.bind(this));
		this.fireEvent('onOpen');
	},

	build: function() {
		this.addEvent('onBlur', function() {
			this.focused = false;
			this.box.removeClass('remooz-box-focus').setStyle('z-index', ReMooz.options.zIndex);
		}, true);
		this.addEvent('onFocus', function() {
			this.focused = true;
			this.box.addClass('remooz-box-focus').setStyle('z-index', ReMooz.options.zIndexFocus);
		}, true);

		var classes = ['remooz-box', 'remooz-type-' + this.options.type, 'remooz-engine-' + Browser.Engine.name + Browser.Engine.version];
		if (this.options.className) classes.push(this.options.className);
		this.box = new Element('div', {
			'class': classes.join(' '),
			'styles': {
				'display': 'none',
				'top': 0,
				'left': 0,
				'zIndex': ReMooz.options.zIndex
			}
		});

		this.tweens = {
			'box': new Fx.Morph(this.box, $merge({
					'duration': 400,
					'unit': 'px',
					'transition': Fx.Transitions.Quart.easeOut,
					'chain': 'cancel'
				}, this.options.resizeOptions)
			),
			'fade': new Fx.Tween(null, $merge({
					'property': 'opacity',
					'duration': (Browser.Engine.trident) ? 0 : 300,
					'chain': 'cancel'
				}, this.options.fxOptions)).addEvents({
					'onComplete': function() {
						if (!this.element.get('opacity')) this.element.setStyle('display', 'none');
					},
					'onStart': function() {
						if (!this.element.get('opacity')) this.element.setStyle('display', '');
					}
				}
			)
		};
		this.tweens.fade.element = $$();

		if (this.options.shadow) {
			if (Browser.Engine.webkit420) {
				this.box.setStyle('-webkit-box-shadow', '0 0 10px rgba(0, 0, 0, 0.7)');
			} else if (!Browser.Engine.trident4) {
				var shadow = new Element('div', {'class': 'remooz-bg-wrap'}).inject(this.box);
				['n', 'ne', 'e', 'se', 's', 'sw', 'w', 'nw'].each(function(dir) {
					new Element('div', {'class': 'remooz-bg remooz-bg-' + dir}).inject(shadow);
				});
				this.tweens.bg = new Fx.Tween(shadow, {
					'property': 'opacity',
					'chain': 'cancel'
				}).set(0);
				this.addEvent(this.options.shadow, this.tweens.bg.set.bind(this.tweens.bg, 1), true);
				this.addEvent('onClose', this.tweens.bg.set.bind(this.tweens.bg, 0), true);
			}
		}

		if (this.options.closer) {
			var closer = new Element('a', {
				'class': 'remooz-btn-close',
				'events': {'click': this.bound.close}
			}).inject(this.box);
			this.tweens.fade.element.push(closer);
		}
		this.body = new Element('div', {'class': 'remooz-body'}).inject(this.box);

		var title = this.options.title || this.options.generateTitle.call(this, this.element);
		if (title) { // thx ie6
			var title = new Element('div', {'class': 'remooz-title'}).adopt(
				new Element('div', {'class': 'remooz-title-bg', 'opacity': this.options.opacityTitle}),
				new Element('div', {'class': 'remooz-title-content'}).adopt(title)
			).inject(this.box);
			this.tweens.fade.element.push(title);
		}
		this.tweens.fade.set(0).fireEvent('onComplete');

		this.drag = new Drag.Move(this.box, {
			'snap': 15,
			'preventDefault': true,
			'onBeforeStart': function() {
				if (!this.focused && !this.loading) ReMooz.focus(this);
				else if (this.loading || this.options.closeOnClick) this.box.addEvent('mouseup', this.bound.dragClose);
			}.bind(this),
			'onSnap': function() {
				this.box.removeEvent('mouseup', this.bound.dragClose);
				if (!this.options.dragging) this.drag.stop();
				else this.box.addClass('remooz-box-dragging');
			}.bind(this),
			'onComplete': function() {
				this.box.removeClass('remooz-box-dragging');
			}.bind(this)
		});
		this.drag.detach();

		this.fireEvent('onBuild', this.box, this.element);
		this.box.inject(this.element.getDocument().body);
	}

});

ReMooz.factory = function(extended) {
	return $extend(this, extended);
};

ReMooz.factory(new Options).factory({

	options: {
		zIndex: 41,
		zIndexFocus: 42,
		query: 'a.remooz',
		modal: false
	},

	assign: function(elements, options) {
		return $$(elements).map(function(element) {
			return new ReMooz(element, options);
		}, this);
	},

	stack: [],

	open: function(obj) {
		var last = this.stack.getLast();
		this.focus(obj);
		if (last && this.options.modal) last.close();
	},

	close: function(obj) {
		var length = this.stack.length - 1;
		if (length > 1 && this.stack[length] == obj) this.focus(this.stack[length - 1]);
		this.stack.erase(obj);
	},

	focus: function(obj) {
		var last = this.stack.getLast();
		obj.fireEvent('onFocus', [obj]);
		if (last == obj) return;
		if (last) last.fireEvent('onBlur', [last]);
		this.stack.erase(obj).push(obj);
	}

});