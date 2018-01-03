<?xml version='1.0'?>
<!DOCTYPE xsl:stylesheet [
<!ENTITY nbsp "&#160;">
<!ENTITY copy "&#169;">
<!ENTITY middot "&#183;">
]>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/html/chunk.xsl"/>

<!-- see: http://docbook.sourceforge.net/release/xsl/current/doc/html/index.html -->

<!-- we pass that from the outside so that we can use the same .xsl with Saxon and xsltproc -->
<!-- <xsl:param name="use.extensions" select="1"></xsl:param> -->

<xsl:param name="chunk.quietly" select="1"></xsl:param>

<xsl:param name="tablecolumns.extension" select="1"></xsl:param>
<xsl:param name="graphicsize.extension" select="1"></xsl:param>
<xsl:param name="link.to.self.for.mediaobject" select="1" />
<xsl:param name="default.table.width" select="'100%'"></xsl:param>

<xsl:param name="make.valid.html" select="1"></xsl:param>
<xsl:param name="html.cleanup" select="1"></xsl:param>
<xsl:param name="make.clean.html" select="1"></xsl:param>
<xsl:param name="html.stylesheet.type">text/css</xsl:param>
<xsl:param name="html.stylesheet">/theme/css/normalize.css /theme/css/skeleton.css /theme/css/darktable.css /theme/css/usermanual.css</xsl:param>
<xsl:param name="docbook.css.source" />
<xsl:param name="docbook.css.link" select="0"></xsl:param>
<xsl:param name="generate.meta.abstract" select="1"></xsl:param>

<xsl:param name="header.rule" select="0"></xsl:param>
<xsl:param name="footer.rule" select="0"></xsl:param>
<xsl:param name="navig.showtitles">1</xsl:param>

<xsl:param name="chunker.output.indent">yes</xsl:param>
<xsl:param name="chunker.output.encoding">UTF-8</xsl:param>

<xsl:param name="section.autolabel" select="1"></xsl:param>
<xsl:param name="section.autolabel.max.depth" select="3"></xsl:param>
<xsl:param name="section.label.includes.component.label" select="1"></xsl:param>
<xsl:param name="draft.mode" select="1"></xsl:param>

<xsl:param name="use.id.as.filename" select="1"></xsl:param>
<xsl:param name="chunk.section.depth" select="2"></xsl:param>

<xsl:param name="variablelist.as.blocks" select="1"></xsl:param>

<xsl:param name="usermanual_languages"></xsl:param>

<!-- see: http://www.sagehill.net/docbookxsl/index.html -->


<!-- breacrumb navigation -->
<xsl:template name="breadcrumbs">
  <xsl:param name="this.node" select="."/>
  <div class="breadcrumbs">
    <xsl:for-each select="$this.node/ancestor::*">
      <span class="breadcrumb-link">
        <a>
          <xsl:attribute name="href">
            <xsl:call-template name="href.target">
              <xsl:with-param name="object" select="."/>
              <xsl:with-param name="context" select="$this.node"/>
            </xsl:call-template>
          </xsl:attribute>
          <xsl:apply-templates select="." mode="title.markup"/>
        </a>
      </span>
      <xsl:text> &gt; </xsl:text>
    </xsl:for-each>
    <!-- And display the current node, but not as a link -->
    <span class="breadcrumb-node">
      <xsl:apply-templates select="$this.node" mode="title.markup"/>
    </span>
  </div>
</xsl:template>

<xsl:template name="user.header.content">
  <xsl:call-template name="breadcrumbs"/>
</xsl:template>


<!-- usermanual navigation -->
<xsl:template name="header.navigation">
  <xsl:param name="prev" select="/foo"/>
  <xsl:param name="next" select="/foo"/>
  <xsl:param name="nav.context"/>

  <xsl:variable name="home" select="/*[1]"/>
  <xsl:variable name="up" select="parent::*"/>

  <xsl:variable name="row" select="count($prev) &gt; 0
                                    or (count($up) &gt; 0
                                        and generate-id($up) != generate-id($home)
                                        and $navig.showtitles != 0)
                                    or count($next) &gt; 0"/>

  <div class="navheader">
    <div class="usermanual_nav_clear" />

    <xsl:if test="$row">
      <div class="usermanual_nav_left">
        <xsl:if test="count($prev)>0">
          <a accesskey="p">
            <xsl:attribute name="href">
              <xsl:call-template name="href.target">
                <xsl:with-param name="object" select="$prev"/>
              </xsl:call-template>
            </xsl:attribute>
            <xsl:text>&lt;&nbsp;</xsl:text>
            <xsl:call-template name="navig.content">
              <xsl:with-param name="direction" select="'prev'"/>
            </xsl:call-template>
          </a>
        </xsl:if>
      </div>

      <div class="usermanual_nav_right">
        <xsl:text>&#160;</xsl:text>
        <xsl:if test="count($next)>0">
          <a accesskey="n">
            <xsl:attribute name="href">
              <xsl:call-template name="href.target">
                <xsl:with-param name="object" select="$next"/>
              </xsl:call-template>
            </xsl:attribute>
            <xsl:call-template name="navig.content">
              <xsl:with-param name="direction" select="'next'"/>
            </xsl:call-template>
            <xsl:text>&nbsp;&gt;</xsl:text>
          </a>
        </xsl:if>
      </div>

      <div class="usermanual_nav_center">
        <xsl:choose>
          <xsl:when test="count($up) > 0
                          and generate-id($up) != generate-id($home)
                          and $navig.showtitles != 0">
            <xsl:apply-templates select="$up" mode="object.title.markup"/>
          </xsl:when>
          <xsl:otherwise>&#160;</xsl:otherwise>
        </xsl:choose>
      </div>

    </xsl:if> <!-- $row -->

  </div> <!-- navheader -->
</xsl:template>


<xsl:template name="footer.navigation">
  <xsl:param name="prev" select="/foo"/>
  <xsl:param name="next" select="/foo"/>
  <xsl:param name="nav.context"/>

  <xsl:variable name="home" select="/*[1]"/>
  <xsl:variable name="up" select="parent::*"/>

  <xsl:variable name="row1" select="count($prev) &gt; 0
                                    or count($up) &gt; 0
                                    or count($next) &gt; 0"/>

  <xsl:variable name="row2" select="($prev and $navig.showtitles != 0)
                                    or (generate-id($home) != generate-id(.)
                                        or $nav.context = 'toc')
                                    or ($chunk.tocs.and.lots != 0
                                        and $nav.context != 'toc')
                                    or ($next and $navig.showtitles != 0)"/>

  <div class="navfooter">
    <xsl:if test="$row1 or $row2">
      <xsl:if test="$row1">

        <div class="usermanual_nav_left">
          <xsl:if test="count($prev)>0">
            <a accesskey="p">
              <xsl:attribute name="href">
                <xsl:call-template name="href.target">
                  <xsl:with-param name="object" select="$prev"/>
                </xsl:call-template>
              </xsl:attribute>
              <xsl:text>&lt;&nbsp;</xsl:text>
              <xsl:call-template name="navig.content">
                <xsl:with-param name="direction" select="'prev'"/>
              </xsl:call-template>
            </a>
          </xsl:if>
        </div>

        <div class="usermanual_nav_right">
          <xsl:if test="count($next)>0">
            <a accesskey="n">
              <xsl:attribute name="href">
                <xsl:call-template name="href.target">
                  <xsl:with-param name="object" select="$next"/>
                </xsl:call-template>
              </xsl:attribute>
              <xsl:call-template name="navig.content">
                <xsl:with-param name="direction" select="'next'"/>
              </xsl:call-template>
              <xsl:text>&nbsp;&gt;</xsl:text>
            </a>
          </xsl:if>
        </div>

        <div class="usermanual_nav_center">
          <xsl:choose>
            <xsl:when test="count($up)&gt;0
                            and generate-id($up) != generate-id($home)">
              <a accesskey="u">
                <xsl:attribute name="href">
                  <xsl:call-template name="href.target">
                    <xsl:with-param name="object" select="$up"/>
                  </xsl:call-template>
                </xsl:attribute>
                <xsl:call-template name="navig.content">
                  <xsl:with-param name="direction" select="'up'"/>
                </xsl:call-template>
              </a>
            </xsl:when>
            <xsl:otherwise>&nbsp;</xsl:otherwise>
          </xsl:choose>
        </div>

      </xsl:if> <!-- $row1 -->

      <xsl:if test="$row2">

        <div class="usermanual_nav_left">
          <xsl:if test="$navig.showtitles != 0">
            <xsl:apply-templates select="$prev" mode="object.title.markup"/>
          </xsl:if>
        </div>

        <div class="usermanual_nav_right">
          <xsl:if test="$navig.showtitles != 0">
            <xsl:apply-templates select="$next" mode="object.title.markup"/>
          </xsl:if>
        </div>

        <div class="usermanual_nav_center">
          <xsl:choose>
            <xsl:when test="$home != . or $nav.context = 'toc'">
              <a accesskey="h">
                <xsl:attribute name="href">
                  <xsl:call-template name="href.target">
                    <xsl:with-param name="object" select="$home"/>
                  </xsl:call-template>
                </xsl:attribute>
                <xsl:call-template name="navig.content">
                  <xsl:with-param name="direction" select="'home'"/>
                </xsl:call-template>
              </a>
              <xsl:if test="$chunk.tocs.and.lots != 0 and $nav.context != 'toc'">
                <xsl:text>&nbsp;|&nbsp;</xsl:text>
              </xsl:if>
            </xsl:when>
            <xsl:otherwise>&nbsp;</xsl:otherwise>
          </xsl:choose>

          <xsl:if test="$chunk.tocs.and.lots != 0 and $nav.context != 'toc'">
            <a accesskey="t">
              <xsl:attribute name="href">
                <xsl:value-of select="$chunked.filename.prefix"/>
                <xsl:apply-templates select="/*[1]"
                                      mode="recursive-chunk-filename">
                  <xsl:with-param name="recursive" select="true()"/>
                </xsl:apply-templates>
                <xsl:text>-toc</xsl:text>
                <xsl:value-of select="$html.ext"/>
              </xsl:attribute>
              <xsl:call-template name="gentext">
                <xsl:with-param name="key" select="'nav-toc'"/>
              </xsl:call-template>
            </a>
          </xsl:if>
        </div>

      </xsl:if> <!-- $row2 -->
    </xsl:if> <!-- $row1 or $row2 -->
  </div>

</xsl:template>


<!-- extra <header> fields -->
<xsl:template name="dt_header_title">
  <xsl:variable name="title">
    <xsl:apply-templates select="." mode="title.markup" />
  </xsl:variable>
  <xsl:attribute name="content">
    <xsl:text>&#8220;</xsl:text>
    <xsl:value-of select="$title" />
    <xsl:text>&#8221; in the darktable usermanual</xsl:text>
  </xsl:attribute>
</xsl:template>


<xsl:template name="dt_header_filename">
  <xsl:text>https://www.darktable.org/usermanual/</xsl:text>
  <xsl:call-template name="href.target">
    <xsl:with-param name="object" select="."/>
  </xsl:call-template>
</xsl:template>


<xsl:template name="dt_header_filename_href">
  <xsl:attribute name="href">
    <xsl:call-template name="dt_header_filename" />
  </xsl:attribute>
</xsl:template>


<xsl:template name="dt_header_filename_content">
  <xsl:attribute name="content">
    <xsl:call-template name="dt_header_filename" />
  </xsl:attribute>
</xsl:template>


<xsl:template name="system.head.content">
  <meta name="viewport" content="width=device-width, initial-scale=1" />

  <meta name="twitter:card" content="summary_large_image" />
  <meta name="twitter:site" content="@darktable_org" />

  <meta itemprop="name">
    <xsl:call-template name="dt_header_title"/>
  </meta>
  <meta name="twitter:title">
    <xsl:call-template name="dt_header_title"/>
  </meta>
  <meta property="og:title">
    <xsl:call-template name="dt_header_title"/>
  </meta>
  <meta itemprop="headline">
    <xsl:call-template name="dt_header_title"/>
  </meta>

  <meta property="og:type" content="website" />
  <meta property="og:site_name" content="darktable.org" />

  <!--  TODO: do we want to randomize the image to use at compile time?  -->
  <meta itemprop="image" content="https://www.darktable.org/theme/lede-usermanual.jpg" />
  <meta property="og:image" content="https://www.darktable.org/theme/images/lede-usermanual.jpg" />
  <meta name="twitter:image" content="https://www.darktable.org/theme/images/lede-usermanual.jpg" />

  <meta itemprop="description">
    <xsl:call-template name="dt_header_title"/>
  </meta>
  <meta property="og:description">
    <xsl:call-template name="dt_header_title"/>
  </meta>
  <meta name="twitter:description">
    <xsl:call-template name="dt_header_title"/>
  </meta>

  <meta name="twitter:creator" content="@darktable_org" />

  <link rel="canonical" itemprop="url">
    <xsl:call-template name="dt_header_filename_href" />
  </link>
  <meta property="og:url">
    <xsl:call-template name="dt_header_filename_content" />
  </meta>
  <meta property="url">
    <xsl:call-template name="dt_header_filename_content" />
  </meta>

  <link rel="shortcut icon" href="/favicon.ico" />

</xsl:template>


<!-- the site title -->
<xsl:template name="user.head.title">
  <xsl:param name="node" select="."/>
  <xsl:param name="title"/>

  <title>
    <xsl:copy-of select="$title"/>
    <xsl:text> | usermanual | darktable</xsl:text>
  </title>
</xsl:template>


<!-- the site navigation / header / lede -->
<xsl:template name="user.header.navigation">

  <nav id="menu">
    <ul>
      <li><a href="/news/">news</a></li>
      <li><a href="/blog/">blog</a></li>
      <li><a href="/about/">about</a></li>
      <li><a href="/install/">install</a></li>
      <li><a href="/resources/">resources</a></li>
      <li><a href="/development/">development</a></li>
      <li><a href="/contact/">contact</a></li>
      <li>
        <form id="searchform" action="/search.html">
          <svg viewBox="0 0 16 16" height="16" width="16" style="vertical-align: middle;">
            <g transform="translate(0 -1036.4)">
              <ellipse style="color-rendering:auto;color:#000000;shape-rendering:auto;solid-color:#000000;stroke:#fff;stroke-width:2;fill:none;" rx="5.0368" ry="5.0368" cy="1042.4" cx="9.9632"/>
              <path style="stroke:#fff;stroke-width:3;fill:none" d="m1.0607 1051.3 5.114-5.114"/>
            </g>
          </svg>
          <input type="text" class="searchbox" name="q" id="tipue_search_input" placeholder="search" />
        </form>
      </li>
    </ul>
  </nav>

  <span id="logo">
    <a href="/" title="darktable.org">
      <img src="/theme/images/darktable-logo-name-480w.png" />
    </a>
  </span>

  <section class="lede-bg">
    <img src="/theme/images/lede-usermanual.jpg" alt="darktable page lede image" />
  </section>
  <section class="lede page">
    <img class="lede-img" src="/theme/images/lede-usermanual.jpg" alt="darktable page lede image" width="960" height="402" />
  </section>

</xsl:template>


<!-- the page footer -->
<xsl:template name="user.footer.navigation">
  <section class="footer">
    <div class="container">
      <div class="row">

        <div class="six columns">
          <ul class="sitenav">
            <li><a href="/">darktable</a></li>
            <li><a href="/about/">about</a></li>
            <li><a href="/about/faq/">faq</a></li>
            <li><a href="/about/features/">features</a></li>
            <li><a href="/about/meta/">meta</a></li>
            <li><a href="/about/screenshots/">screenshots</a></li>
            <li><a href="/contact/">contact</a></li>
            <li><a href="/credits/">credits</a></li>
            <li><a href="/development/">development</a></li>
            <li><a href="/install/">install</a></li>
            <li><a href="/resources/">resources</a></li>
            <li><a href="/resources/camera-support//">camera&nbsp;support</a></li>
          </ul>
        </div>

        <div class="six columns smalltext">
          <p>
            <svg xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://www.w3.org/2000/svg" height="12" width="12" version="1.1" xmlns:cc="http://creativecommons.org/ns#" xmlns:dc="http://purl.org/dc/elements/1.1/" viewBox="0 0 48 48.000001">
              <g transform="translate(0 -1004.4)">
                <circle style="color-rendering:auto;color:#000000;isolation:auto;mix-blend-mode:normal;shape-rendering:auto;solid-color:#000000;image-rendering:auto" cx="6.4379" cy="1045.9" r="6.5" fill="#787878"/>
                <path style="color-rendering:auto;color:#000000;isolation:auto;mix-blend-mode:normal;shape-rendering:auto;solid-color:#000000;image-rendering:auto" fill="none" stroke="#787878" stroke-width="9" d="m-0.062104 1026.3a26.142 26.142 0 0 1 26.142 26.1"/>
                <path style="color-rendering:auto;color:#000000;isolation:auto;mix-blend-mode:normal;shape-rendering:auto;solid-color:#000000;image-rendering:auto" fill="none" stroke-opacity=".94118" stroke="#787878" stroke-width="10" d="m0.0010728 1009.3a43.026 43.026 0 0 1 43.026 43.1"/>
              </g>
            </svg>
            RSS Feeds:<br />
            <a href="/news.rss.xml" type="application/rss+xml" rel="alternate" title="darktable Categories RSS Feed" >news feed</a>
            &middot;
            <a href="/blog.rss.xml" type="application/rss+xml" rel="alternate" title="darktable Categories RSS Feed" >blog feed</a>
          </p>
          <p>Connect with us on:<br />
            <a href="http://www.facebook.com/darktable" title="darktable on Facebook">Facebook</a>
            &middot;
            <a href="http://www.flickr.com/groups/darktable/" title="darktable on Flickr">Flickr</a>
            &middot;
            <a href="http://twitter.com/#!/darktable_org" title="darktable on Twitter">Twitter</a>
            &middot;
            <a href="http://www.google.com/+darktable" title="darktable on Google+">Google+</a>
          </p>

          <div class="badge">
            <a class="badge-logo" target="_blank" rel="license" title="GPLv3.0" href="http://www.gnu.org/licenses/gpl-3.0.html">
              <img alt="GPLv3.0" src="/theme/images/gplv3-88x31.png" />
            </a>
            <div class="badge-text">
              darktable is released under the <a href="http://www.gnu.org/licenses/gpl-3.0.html">GPL 3.0</a><br />
              <a href="/">&copy;2017 by the darktable team.</a> »
              <a href="/credits/">credits</a>
            </div>
          </div>

          <div class="badge clearfix">
            <a class="badge-logo" target="_blank" rel="license" href="http://creativecommons.org/licenses/by-nc-sa/3.0/">
              <img alt="Creative Commons License" src="/theme/images/CCBYSANC_88x31.png" />
            </a>
            <div class="badge-text">
              Unless otherwise stated, this website and images are licensed <a rel="license" target="_blank" href="http://creativecommons.org/licenses/by-nc-sa/3.0/">Creative Commons BY-NC-SA 3.0 License</a>.
            </div>
          </div>

        </div>

      </div>

      <div class="row pixlslove">
        <div class="four columns offset-by-four">
          <p><a href="https://pixls.us">pixls.us</a> <span class="heart">♡</span>’s darktable</p>
        </div>
      </div>

    </div>
  </section>
</xsl:template>


<!-- the links to other languages -->
<xsl:template name="format.dt_language">
  <xsl:param name="language" />
  <xsl:call-template name="l10n.language.name">
    <xsl:with-param name="lang" select="$language" />
  </xsl:call-template>
<!--   <xsl:text> (</xsl:text> -->
<!--   <xsl:value-of select="$language" /> -->
<!--   <xsl:text>)</xsl:text> -->
</xsl:template>


<xsl:template name="output.dt_language">
  <xsl:param name="language" />
  <xsl:variable name="current_language">
    <xsl:call-template name="l10n.language"/>
  </xsl:variable>

  <li class="language_list_entry">
  <xsl:choose>
    <xsl:when test="$language = $current_language">
      <xsl:call-template name="format.dt_language">
        <xsl:with-param name="language" select="$language" />
      </xsl:call-template>
    </xsl:when>
    <xsl:otherwise>
      <a>
        <xsl:attribute name="href">
          <xsl:text>../</xsl:text>
          <xsl:value-of select="$language" />
          <xsl:text>/</xsl:text>
          <xsl:call-template name="href.target" />
        </xsl:attribute>
        <xsl:call-template name="format.dt_language">
          <xsl:with-param name="language" select="$language" />
        </xsl:call-template>
      </a>
    </xsl:otherwise>
  </xsl:choose>
  </li>

</xsl:template>

<xsl:template name="output.dt_languages">
  <xsl:param name="languages" select="''"/>
  <xsl:choose>
    <xsl:when test="contains($languages, ' ')">
      <xsl:variable name="language" select="substring-before($languages, ' ')"/>
      <xsl:call-template name="output.dt_language">
        <xsl:with-param name="language" select="$language"/>
      </xsl:call-template>
      <xsl:call-template name="output.dt_languages">
        <xsl:with-param name="languages" select="substring-after($languages, ' ')"/>
      </xsl:call-template>
    </xsl:when>
    <xsl:when test="$languages != ''">
      <xsl:call-template name="output.dt_language">
        <xsl:with-param name="language" select="$languages"/>
      </xsl:call-template>
    </xsl:when>
  </xsl:choose>
</xsl:template>

<xsl:template name="dt_languages">
  <xsl:if test="$usermanual_languages != ''">
    <ul class="language_list">
      <xsl:call-template name="output.dt_languages">
        <xsl:with-param name="languages" select="normalize-space($usermanual_languages)"/>
      </xsl:call-template>
    </ul>
  </xsl:if>
</xsl:template>

<!--
  Wrap this to add anchor links.
-->
<xsl:template name="section.heading">
  <xsl:param name="section" select="."/>
  <xsl:param name="level" select="1"/>
  <xsl:param name="allow-anchors" select="1"/>
  <xsl:param name="title"/>
  <xsl:param name="class" select="'title'"/>

  <xsl:variable name="id">
    <xsl:choose>
      <!-- Make sure the subtitle doesn't get the same id as the title -->
      <xsl:when test="self::subtitle">
        <xsl:call-template name="object.id">
          <xsl:with-param name="object" select="."/>
        </xsl:call-template>
      </xsl:when>
      <!-- if title is in an *info wrapper, get the grandparent -->
      <xsl:when test="contains(local-name(..), 'info')">
        <xsl:call-template name="object.id">
          <xsl:with-param name="object" select="../.."/>
        </xsl:call-template>
      </xsl:when>
      <xsl:otherwise>
        <xsl:call-template name="object.id">
          <xsl:with-param name="object" select=".."/>
        </xsl:call-template>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:variable>

  <!-- HTML H level is one higher than section level -->
  <xsl:variable name="hlevel">
    <xsl:choose>
      <!-- highest valid HTML H level is H6; so anything nested deeper
           than 5 levels down just becomes H6 -->
      <xsl:when test="$level &gt; 5">6</xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="$level + 1"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:variable>
  <xsl:element name="h{$hlevel}">
    <xsl:attribute name="class"><xsl:value-of select="$class"/></xsl:attribute>
    <xsl:if test="$css.decoration != '0'">
      <xsl:if test="$hlevel&lt;3">
        <xsl:attribute name="style">clear: both</xsl:attribute>
      </xsl:if>
    </xsl:if>
    <xsl:if test="$allow-anchors != 0">
      <xsl:call-template name="anchor">
        <xsl:with-param name="node" select="$section"/>
        <xsl:with-param name="conditional" select="0"/>
      </xsl:call-template>
    </xsl:if>
    <xsl:copy-of select="$title"/>
    <xsl:if test="$hlevel&lt;=4">
      <a class="anchor">
        <xsl:attribute name="href">
          <xsl:text>#</xsl:text>
          <xsl:call-template name="object.id">
            <xsl:with-param name="object" select="$section"/>
          </xsl:call-template>
        </xsl:attribute>
        <xsl:text>¶</xsl:text>
      </a>
    </xsl:if>
  </xsl:element>
</xsl:template>


<!--
  we need to customize chunk-element-content to wrap the page content.
  we keep the other parts separate though to make it easier to update.
  when updating keep in mind that we also changed the order of elements!
-->
<xsl:template name="chunk-element-content">
  <xsl:param name="prev"/>
  <xsl:param name="next"/>
  <xsl:param name="nav.context"/>
  <xsl:param name="content">
    <xsl:apply-imports/>
  </xsl:param>

  <xsl:call-template name="user.preroot"/>

  <html>
    <xsl:call-template name="root.attributes"/>
    <xsl:call-template name="html.head">
      <xsl:with-param name="prev" select="$prev"/>
      <xsl:with-param name="next" select="$next"/>
    </xsl:call-template>

    <body>
      <xsl:call-template name="body.attributes"/>

      <xsl:call-template name="user.header.navigation">
        <xsl:with-param name="prev" select="$prev"/>
        <xsl:with-param name="next" select="$next"/>
        <xsl:with-param name="nav.context" select="$nav.context"/>
      </xsl:call-template>


      <section class="page">

        <div class="container title">
          <div class="row">
            <div class="column">
              <xsl:call-template name="dt_languages"/>
              <h1 class="page-title"><xsl:apply-templates select="." mode="title.markup"/></h1>
            </div>
          </div>
        </div>

        <div class="container content">
          <div class="row">
            <div class="column">

              <xsl:call-template name="user.header.content"/>

              <xsl:call-template name="header.navigation">
                <xsl:with-param name="prev" select="$prev"/>
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="nav.context" select="$nav.context"/>
              </xsl:call-template>

              <div id="search_content">
                <xsl:copy-of select="$content"/>
              </div>

              <xsl:call-template name="footer.navigation">
                <xsl:with-param name="prev" select="$prev"/>
                <xsl:with-param name="next" select="$next"/>
                <xsl:with-param name="nav.context" select="$nav.context"/>
              </xsl:call-template>

            </div>
          </div>
        </div>
      </section>

      <xsl:call-template name="user.footer.content"/>

      <xsl:call-template name="user.footer.navigation">
        <xsl:with-param name="prev" select="$prev"/>
        <xsl:with-param name="next" select="$next"/>
        <xsl:with-param name="nav.context" select="$nav.context"/>
      </xsl:call-template>
    </body>
  </html>
  <xsl:value-of select="$chunk.append"/>
</xsl:template>

</xsl:stylesheet>
