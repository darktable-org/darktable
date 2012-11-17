/* LightZAP v2.52b (sandbox)
by Szalai Mihaly - http://dragonzap.szunyi.com
original by Lokesh Dhakar (lightbox) - http://placeWidthw.lokeshdhakar.com

For more information, visit:
http://dragonzap.szunyi.com/index.php?e=page&al=lightzap&l=en

Licensed under the Creative Commons Attribution 2.5 License - http://creativecommons.org/licenses/by/2.5/
- free for use in both personal and commercial projects
- attribution requires leaving author name, author link, and the license info intact
	
Thanks
- Scott Upton(uptonic.com), Peter-Paul Koch(quirksmode.com), and Thomas Fuchs(mir.aculo.us) for ideas, libs, and snippets.
- Artemy Tregubenko (arty.name) for cleanup and help in updating to latest proto-aculous in v2.05.
- Szalai Mihaly (dragonzap.szunyi.com), automatic image resize for screen, fullscreen viewer,  print button, download button, like button and new design.
- Cross1492 (placeWidthw.subviz.com) external link idea
*/
(function () {
	var $ = jQuery, album = [], currentImageIndex = 0, LightZAP, LightZAPOptions, $lightzap, $container, $image, lz;
	var windowWidth, windowHeight, originalWidth, originalHeight, isfull = false, marginWidth = -1, marginHeight = -1;
	var pfx = ["webkit", "moz", "ms", "o", ""];
	LightZAPOptions = (function () {
		function LightZAPOptions() {
			// Settings
			this.resizeDuration = 500;	
			this.fadeDuration = 500;
			this.imagetext = "";	//"Image "
			this.oftext = " / ";	//" of "
			this.bytext = "by";
			// Optional
			this.print = false;
			this.download = true;
			this.like = false;
		}
		return LightZAPOptions;
	})();
	LightZAP = (function () {
		function LightZAP(options) {
			lz = this;
			lz.options = options;

			//Set links
			$("body").on("click", "a[rel^=lightzap], area[rel^=lightzap]", function (e) {
				lz.start($(e.currentTarget));
				return false;
			});
			$("body").on("click", "a[rel^=lightbox], area[rel^=lightbox]", function (e) {
				lz.start($(e.currentTarget));
				return false;
			});

			//Build lightzap
			$("<div/>", { id: "lightzap" }).append (
				$("<div/>", { "class": "lz-container" }).append	(
					$("<div/>", { "class": "lz-loader" }),
					$("<img/>", {"class": "lz-image" }),
					$("<div/>", { "class": "lz-nav" }).append (
						$("<a/>", { "class": "lz-prev" }),
						$("<a/>", { "class": "lz-next" })
					),
					$("<div/>", { "class": "lz-float lz-buttonContainer" }).append (
						$("<div/>", { "class": "lz-more lz-button" }),
						$("<div/>", { "class": "lz-close lz-button" })
					),
					$("<div/>", { "class": "lz-labelContainer" }).append (
						$("<div/>", { "class": "lz-float lz-caption" }),
						$("<div/>", { "class": "lz-float lz-desc" }),
						$("<div/>", { "class": "lz-float lz-resolution" }),
						$("<div/>", { "class": "lz-float lz-number" }),
						$("<a/>", { "class": "lz-float lz-by" })
					)
				)
			).appendTo($("body"));

			//Set varibles
			$lightzap = $("#lightzap").hide();//and hide
			$image = $lightzap.find(".lz-image");
			$container = $lightzap.find(".lz-container");
			$lightzap.width(windowWidth).height(windowHeight);

			//FullScreen
			var pfx0 = ["IsFullScreen", "FullScreen"];
			var pfx1 = ["CancelFullScreen", "RequestFullScreen"];
			var p = 0, k, m, t = "undefined";
			while (p < pfx.length && !document[m]) {
				k = 0;
				while (k < pfx0.length) {
					m = pfx0[k];
					if (pfx[p] == "") {
						m = m.substr(0, 1).toLowerCase() + m.substr(1);
						pfx1[0] = pfx1[0].substr(0, 1).toLowerCase() + pfx1[0].substr(1);
						pfx1[1] = pfx1[1].substr(0, 1).toLowerCase() + pfx1[1].substr(1);
					}
					m = pfx[p] + m;
					t = typeof document[m];
					if (t != "undefined") {
						pfx = [pfx[p]+pfx1[0], pfx[p]+pfx1[1], m];
						p = 2;
						break;
					}
					k++;
				}
				p++;
			}

			//Buttons
			if (p != 3)
				pfx = false;
			else {
				$("<div/>", {"class": "lz-fullScreen lz-button"}).insertBefore(".lz-close");
				$lightzap.find(".lz-fullScreen").on("click", function () {
					if (isfull)
						document[pfx[0]]();
					else
						document.getElementById("lightzap")[pfx[1]]();
				});
			}
			if (this.options.like)
			{
				$("<div/>", {"class" : "lz-like lz-button", "data-layout": "button_count"}).insertBefore(".lz-close");
				$lightzap.find(".lz-like").on("click", lz.like);
			}
			if (this.options.print)	{
				$("<div/>", {"class": "lz-button lz-print"}).insertBefore(".lz-close");
				$lightzap.find(".lz-print").on("click", lz.print);
			}
			if (this.options.download) {
				$("<div/>", {"class": "lz-button lz-download", "target" : "_blank"}).insertBefore(".lz-close");
				$lightzap.find(".lz-download").on("click", lz.download);
			}

			//Events
			$lightzap.find(".lz-close").on("click",lz.end);
			$lightzap.find(".lz-desc").hide();
			$lightzap.find(".lz-more").on("click", function (e) {
				if ($lightzap.find(".lz-desc").css("display") == "none")
					$lightzap.find(".lz-desc").show();
				else
					$lightzap.find(".lz-desc").hide();
			});
			$lightzap.on("click", function (e) {
				if ($(e.target).attr("id") === "lightzap")
					lz.end();
			});
			$lightzap.find(".lz-prev").on("click", function (e) {
				lz.changeImage(lz.currentImageIndex - 1);
			});
			$lightzap.find(".lz-next").on("click", function (e) {
				lz.changeImage(lz.currentImageIndex + 1);
			});
		};
		LightZAP.prototype.start = function ($link) {
			//Show overlay
			lz.sizeOverlay();
			$lightzap.fadeIn(lz.options.fadeDuration);
			$(window).on("resize", lz.sizeOverlay);
			$("select, object, embed").css("visibility", "hidden");
			
			//Get original margin
			if (marginWidth == -1)
			{
				marginHeight = parseInt($container.css("margin-top"), 10) + parseInt($container.css("margin-bottom"), 10) + parseInt($container.css("padding-top"), 10) + parseInt($container.css("padding-bottom"), 10);
				marginWidth = parseInt($container.css("margin-left"), 10) + parseInt($container.css("margin-right"), 10) + parseInt($container.css("padding-left"), 10) + parseInt($container.css("padding-right"), 10);
			}

			//Create album
			album = [];
			var a, i, imageNumber = 0, _len, _ref;
			if ($link.attr("rel") === "lightzap" || $link.attr("rel") === "lightbox") {
				album.push({
					link: $link.attr("href"),
					title: $link.attr("title"),
					desc: $link.attr("desc"),
					by: $link.attr("by"),
					by_href: $link.attr("by-href")
				});
			} else {
				_ref = $($link.prop("tagName") + "[rel='" + $link.attr("rel") + "']");
				for (i = 0, _len = _ref.length; i < _len; i++) {
					a = _ref[i];
					album.push({
						link: $(a).attr("href"),
						title: $(a).attr("title"),
						desc: $(a).attr("desc"),
						by: $(a).attr("by"),
						by_href: $(a).attr("by-href")
					});
					if ($(a).attr("href") === $link.attr("href"))
						imageNumber = i;
				}
			}
			lz.changeImage(imageNumber);
		};
		LightZAP.prototype.changeImage = function (imageNumber) {
			//Hide other
			$(document).off(".keyboard");
			$lightzap.find(".lz-image, .lz-nav, .lz-prev, .lz-next, .lz-float").hide();
			$lightzap.find(".lz-loader").show();

			//New image
			$container.addClass("animating");
			var preloader = new Image;
			preloader.onload = function () {
				$image.attr("src", album[imageNumber].link);
				originalWidth = preloader.width;
				originalHeight = preloader.height;
				lz.currentImageIndex = imageNumber;
				return lz.getImageSize();
			};
			preloader.src = album[imageNumber].link;
		};
		LightZAP.prototype.sizeOverlay = function () {
			//If chanced size
			if (windowWidth != $(window).width() || windowHeight != $(window).height()) {
				//Set size
				windowWidth = $(window).width();
				windowHeight = $(window).height();
				$lightzap.width(windowWidth).height(windowHeight);

				//Is fullscreen?
				isfull = false;
				if (pfx != false)
					isfull = (typeof document[pfx[2]] == "function" ? document[pfx[2]]() : document[pfx[2]]);
				if (!isfull) {
					if (windowWidth >= screen.width * 0.99 && windowHeight >= screen.height * 0.99)
						isfull = true;
					else
						isfull = false;
				}

				//Set style
				if (isfull) {
					$lightzap.attr("class", "full-screen");
					$lightzap.attr("style", "");
					$container.attr("style", "height: 100%;");
				} else {
					$lightzap.attr("class", "");
					$container.attr("style", "");
				}

				//Update image size
				if (album.length > 0)
					lz.getImageSize();
			}
		};
		LightZAP.prototype.getImageSize = function () {
			$image.css("height", "auto");

			//Sizes
			var placeWidth = windowWidth, placeHeight = windowHeight, imageWidth = originalWidth, imageHeight = originalHeight;
			if (!isfull) {
				placeWidth -= marginWidth;
				placeHeight -= marginHeight;
			} else if (pfx)	{
				placeWidth = screen.width;
				placeHeight = screen.height;
			}
			
			//Calculate optional size
			if (imageWidth > placeWidth) {
				imageHeight = (placeWidth * imageHeight) / imageWidth;
				imageWidth = placeWidth;
			}
			if (imageHeight> placeHeight) {
				imageWidth = (placeHeight * imageWidth) / imageHeight;
				imageHeight = placeHeight;
			}

			//Set fullscreen style
			if (isfull) {
				$image.css("height", imageHeight);
				$image.css("margin", Math.max((placeHeight - imageHeight) / 2, 0) + "px " + Math.max((placeWidth - imageWidth) / 2 , 0) + "px");
				$container.css("margin", "0");
				return lz.showImage();
			}

			//Set box style
			var oldWidth = $container.outerWidth(), oldHeight = $container.outerHeight();
			$image.css("margin", "0");
			$container.css("margin", (windowHeight - imageHeight) / 2 + "px auto 0");
			if ($container.attr("class") == "lz-container animating") {
				if (imageWidth !== oldWidth && imageHeight !== oldHeight) {
					$container.animate({
						width: imageWidth,
						height: imageHeight
					}, lz.options.resizeDuration, "swing");
				} else if (imageWidth !== oldWidth) {
					$container.animate({
						width: imageWidth
					}, lz.options.resizeDuration, "swing");
				} else if (imageHeight !== oldHeight) {
					$container.animate({
						height: imageHeight
					}, lz.options.resizeDuration, "swing");
				}
			} else
				$container.width(imageWidth).height(imageHeight);

			setTimeout(function () {
				$lightzap.find(".lz-prevLink").height(imageHeight);
				$lightzap.find(".lz-nextLink").height(imageHeight);
				lz.showImage();
			}, lz.options.resizeDuration);
		};
		LightZAP.prototype.showImage = function () {
			$lightzap.find(".lz-loader").hide();
			$image.fadeIn(lz.options.fadeDuration*0.75);
			lz.updateNav();
			lz.updateDetails();

			//Preload
			var preloadNext, preloadPrev;
			if (album.length > lz.currentImageIndex + 1) {
				preloadNext = new Image;
				preloadNext.src = album[lz.currentImageIndex + 1].link;
			}
			if (lz.currentImageIndex > 0) {
				preloadPrev = new Image;
				preloadPrev.src = album[lz.currentImageIndex - 1].link;
			}

			//Disable keyboard
			$(document).on("keyup.keyboard", $.proxy(lz.keyboardAction, lz));
		};
		LightZAP.prototype.updateNav = function () {
			$lightzap.find(".lz-nav").fadeIn(lz.options.fadeDuration/2);
			if (lz.currentImageIndex > 0) $lightzap.find(".lz-prev").show();
			if (lz.currentImageIndex < album.length - 1)
				$lightzap.find(".lz-next").show();
		};
		LightZAP.prototype.updateDetails = function () {
			//Caption
			if (typeof album[lz.currentImageIndex].title !== "undefined" && album[lz.currentImageIndex].title !== "")
				$lightzap.find(".lz-caption").html(album[lz.currentImageIndex].title).fadeIn(lz.options.fadeDuration/2);

			//Caption
			if (typeof album[lz.currentImageIndex].desc !== "undefined" && album[lz.currentImageIndex].desc !== "")
			{
				$lightzap.find(".lz-desc").html(album[lz.currentImageIndex].desc).hide();
				$lightzap.find(".lz-more").fadeIn(lz.options.fadeDuration/2);
			}
			else
				$lightzap.find(".lz-more").hide();

			//Counter
			if (album.length > 1)
				$lightzap.find(".lz-number").html(lz.options.imagetext + (lz.currentImageIndex + 1) + lz.options.oftext + album.length).fadeIn(lz.options.fadeDuration/2);
			else
				$lightzap.find(".lz-number").hide();

			//Author
			if (typeof album[lz.currentImageIndex].by !== "undefined" && album[lz.currentImageIndex].by !== "")
				$lightzap.find(".lz-by").html(lz.options.bytext + " <span>" + album[lz.currentImageIndex].by + "</span>").fadeIn(lz.options.fadeDuration/2);
			else
				$lightzap.find(".lz-by").html("");
			if (typeof album[lz.currentImageIndex].by_href !== "undefined" && album[lz.currentImageIndex].by_href !== "")
				$lightzap.find(".lz-by").prop("href",album[lz.currentImageIndex].by_href);
			else
				$lightzap.find(".lz-by").prop("href","");
			
			$lightzap.find(".lz-resolution").html(originalWidth + " x " + originalHeight).fadeIn(lz.options.fadeDuration/2);

			$container.removeClass("animating");
			$lightzap.find(".lz-buttonContainer").fadeIn(lz.options.fadeDuration/2);
		};
		LightZAP.prototype.keyboardAction = function (event) {
			var KEYCODE_ESC, KEYCODE_LEFTARROW, KEYCODE_F11, KEYCODE_RIGHTARROW, key, keycode;
			KEYCODE_ESC = 27;
			KEYCODE_LEFTARROW = 37;
			KEYCODE_RIGHTARROW = 39;
			KEYCODE_F11 = 122;
			keycode = event.keyCode;
			key = String.fromCharCode(keycode).toLowerCase();
			if (keycode === KEYCODE_ESC || key.match(/x|o|c/)){
				lz.end();
			} else if (key == "p" || keycode == KEYCODE_LEFTARROW) {
				if (lz.currentImageIndex != 0)
					lz.changeImage(lz.currentImageIndex - 1);
			} else if (key == "n" || keycode == KEYCODE_RIGHTARROW) {
				if (lz.currentImageIndex != album.length - 1)
					lz.changeImage(lz.currentImageIndex + 1);
			}
		};
		LightZAP.prototype.print = function () {
			win = window.open();
			self.focus();
			win.document.open();
			win.document.write("<html><body stlye='margin:0 auto; padding:0;1>");
			win.document.write("<div align='center' style='margin: 0 auto;'><img src='" + album[lz.currentImageIndex].link + "' style='max-width: 100%; max-height: 90%;'/></div>");
			win.document.write("<strong>" + album[lz.currentImageIndex].title + "</strong></br><i>" + $lightzap.find(".lz-by").html() + "</i>");
			win.document.write("</body></html>");
			win.document.close();
			win.print();
			win.close();
   		};
   		LightZAP.prototype.like = function () {
			if (!window.focus) return true;
			var href = "http://www.facebook.com/sharer/sharer.php?u=" + album[lz.currentImageIndex].link + "&t=" + album[lz.currentImageIndex].title;
			window.open(href, "", 'width=400,height=200,scrollbars=yes');
   		};
   		LightZAP.prototype.download = function () { //Beta version
			if (window.webkitURL) { //Webkit
				var xhr = new XMLHttpRequest();
				xhr.open("GET", album[lz.currentImageIndex].link);
				xhr.responseType = "blob";
				xhr.onreadystatechange = function() { 
					var a = document.createElement("a");
					a.href = (window.URL) ? window.URL.createObjectURL(xhr.response) : window.webkitURL.createObjectURL(xhr.response);
					a.download = album[lz.currentImageIndex].link.substring(album[lz.currentImageIndex].link.lastIndexOf("/") + 1);
					var e = document.createEvent("MouseEvents");
					e.initMouseEvent("click", true, false, window, 0, 0, 0, 0, 0, false, false, false, false, 0, null);
					a.dispatchEvent(e);
				};
				xhr.send();
				return true;
   			} else if (navigator.appName == 'Microsoft Internet Explorer') { //IE
   				win = window.open(album[lz.currentImageIndex].link);
				self.focus();
				win.document.execCommand("SaveAs");
				win.close();
				return true;
			} else { //Opera & Firefox (CANVAS)
				var canvas = document.createElement("canvas");
				document.body.appendChild(canvas);
				if (typeof canvas.getContext != "undefined") {
					try { 
						var context = canvas.getContext("2d");
						canvas.width = Math.min(originalWidth, 1024);
						canvas.height = Math.min(originalHeight, originalHeight / originalWidth * 1024);
						canvas.style.width = canvas.width + "px";
						canvas.style.height = canvas.height + "px";
						context.drawImage(document.getElementsByClassName("lz-image")[0], 0, 0, canvas.width, canvas.height);
						document.location.href = canvas.toDataURL("image/png").replace("image/png", "image/octet-stream");
						document.body.removeChild(canvas);
						return true;
				   	}
				   	catch (err) {
						document.body.removeChild(canvas);
					}
				}					
   			}
   			alert("Sorry, can't download");
   		};
		LightZAP.prototype.end = function () {
			if (isfull && pfx != false)
				document[pfx[0]]();

			album = [];
			$(document).off(".keyboard");
			$(window).off("resize", lz.sizeOverlay);
			$lightzap.fadeOut(lz.options.fadeDuration);
			$("select, object, embed").css("visibility", "visible");
		};
		return LightZAP;
	})();
	$(function () {
		var lightzap, options;
		options = new LightZAPOptions;
		return lightzap = new LightZAP(options);
	});
}).call(this);
