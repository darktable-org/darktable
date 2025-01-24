#!/usr/bin/env perl
use File::Temp qw/ tempfile tempdir /;

$ICONWIDTH = 50;
$ICONHEIGHT = 30;
$BORDER = 4;
$FONTSIZE = 12;
$ALIGNMENT_CROSS = false;

$BG_COLOR = 0.42;
$FG_COLOR = 0.94;

my $srcdir = $ARGV[0];
my $argc = @ARGV;

if($argc != 1){
	print "Usage: show_icons_in_paint_c.pl <src dir>\n";
	exit 1;
}

# create a temporary directory
my $tempdir = tempdir(CLEANUP => 1);

# find all the icons listed in paint.h
open(PAINTH, "<$srcdir/dtgtk/paint.h");
my $comment;
my $lastlinewasacomment = false;
my $code;
my $numberoficons = 0;
my $longeststring;
my @codelines;

while(<PAINTH>){
	chomp;
	next if $_ eq '';
	if($_ =~ /^\/\*\* (.*) \*\//){ # this should be the description of a paint function
		$comment = $1;
		$lastlinewasacomment = true;
	}
	else{
		if($lastlinewasacomment eq true){ # this is the next non-empty line after a comment, so it should be a paint function
			$_ =~ /(dtgtk_cairo_[^(]*)/;
			$code = $1;
			my $description = "$comment ($code)";
			$longeststring = $description if length($longeststring) < length($description);
			$codelines[$numberoficons][0] = $description;
			$codelines[$numberoficons][1] = $code;
			$numberoficons++;
		}
		$lastlinewasacomment = false;
	}
}

close(PAINTH);

# write the .c file
open(TESTC, ">$tempdir/test.c");
print TESTC <<EOF;
#include <cairo.h>
#include "dtgtk/paint.h"
#include "common/darktable.h"
#include "bauhaus/bauhaus.h"

darktable_t darktable;

typedef struct dt_lib_histogram_color_harmony_t
{
  const char *name;
  const int sectors;      // how many sectors
  const float angle[4];   // the angle of the sector center, expressed in fractions of a full turn
  const float length[4];  // the radius of the sector, from 0. to 1., linear scale
} dt_lib_histogram_color_harmony_t;


int main(){
	memset(&darktable, 0, sizeof(darktable_t));
	darktable.bauhaus = (dt_bauhaus_t *)calloc(1, sizeof(dt_bauhaus_t));

	darktable.bauhaus->colorlabels[DT_COLORLABELS_RED]    = (GdkRGBA){.red=230.0/255.0, .green=0.0/255.0,   .blue=0.0/255.0,   .alpha=1.0};
	darktable.bauhaus->colorlabels[DT_COLORLABELS_YELLOW] = (GdkRGBA){.red=255.0/255.0, .green=180.0/255.0, .blue=0.0/255.0,   .alpha=1.0};
	darktable.bauhaus->colorlabels[DT_COLORLABELS_GREEN]  = (GdkRGBA){.red=0.0/255.0,   .green=230.0/255.0, .blue=0.0/255.0,   .alpha=1.0};
	darktable.bauhaus->colorlabels[DT_COLORLABELS_BLUE]   = (GdkRGBA){.red=40.0/255.0,  .green=140.0/255.0, .blue=255.0/255.0, .alpha=1.0};
	darktable.bauhaus->colorlabels[DT_COLORLABELS_PURPLE] = (GdkRGBA){.red=230.0/255.0, .green=0.0/255.0,   .blue=230.0/255.0, .alpha=1.0};

	dt_lib_histogram_color_harmony_t dt_color_harmonies[DT_COLOR_HARMONY_N] =
	{
		{N_("none"),                    0                                                              },
		{N_("monochromatic"),           1, { 0./12.                         }, {0.80                  }},
		{N_("analogous"),               3, {-1./12., 0./12.,  1./12.        }, {0.50, 0.80, 0.50      }},
		{N_("analogous complementary"), 4, {-1./12., 0./12.,  1./12., 6./12.}, {0.50, 0.80, 0.50, 0.50}},
		{N_("complementary"),           2, { 0./12., 6./12                  }, {0.80, 0.50            }},
		{N_("split complementary"),     3, { 0./12., 5./12.,  7./12.        }, {0.80, 0.50, 0.50      }},
		{N_("dyad"),                    2, {-1./12., 1./12                  }, {0.80, 0.80            }},
		{N_("triad"),                   3, { 0./12., 4./12.,  8./12.        }, {0.80, 0.50, 0.50      }},
		{N_("tetrad"),                  4, {-1./12., 1./12.,  5./12., 7./12.}, {0.80, 0.80, 0.50, 0.50}},
		{N_("square"),                  4, { 0./12., 3./12.,  6./12., 9./12.}, {0.80, 0.50, 0.50, 0.50}},
	};

	void *data = &dt_color_harmonies[3];

	cairo_surface_t *surface;
	cairo_t *cr;
	cairo_text_extents_t ext;

	int width, height, textwidth;

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 10, 10);
	cr = cairo_create(surface);

	cairo_select_font_face (cr, "mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size (cr, $FONTSIZE);
	cairo_text_extents (cr, "$longeststring", &ext);
	textwidth = ext.width; width = textwidth+4*$ICONWIDTH+6*$BORDER; height = $numberoficons*($ICONHEIGHT+$BORDER)+$BORDER;

	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cr = cairo_create(surface);

	cairo_select_font_face (cr, "mono", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size (cr, $FONTSIZE);

	cairo_rectangle(cr, 0.0, 0.0, width, height);
	cairo_set_source_rgb(cr, $BG_COLOR, $BG_COLOR, $BG_COLOR);
	cairo_fill(cr);
EOF

# add all the calls to the individual paint functions
my $row = 0;
my $data;

foreach my $line (@codelines){
	my $description = @$line[0];
	$description =~ s/"/\\"/g;
	my $code = @$line[1];

	print $description . "\n";

	$data = "NULL";
	if($description =~ /color_harmony/){
		$data = "&dt_color_harmonies[3]";
	}
	
	# add description text
	print TESTC <<EOF;
	cairo_save(cr);
	cairo_set_source_rgb(cr, $FG_COLOR, $FG_COLOR, $FG_COLOR);
	cairo_move_to (cr, $BORDER, $BORDER+$row*($ICONHEIGHT+$BORDER)+0.8*$ICONHEIGHT);
	cairo_show_text(cr, "$description");
	cairo_restore(cr);
EOF

	# add icons for all four directions
	my $column = 0;
	foreach my $direction ("CPF_DIRECTION_UP", "CPF_DIRECTION_DOWN", "CPF_DIRECTION_LEFT", "CPF_DIRECTION_RIGHT"){
		print TESTC <<EOF;
		cairo_save(cr);
		cairo_rectangle(cr, textwidth+2*$BORDER+$column*($ICONWIDTH+$BORDER), $BORDER+$row*($ICONHEIGHT+$BORDER), $ICONWIDTH, $ICONHEIGHT);
		cairo_set_source_rgb(cr, $BG_COLOR+0.02, $BG_COLOR+0.02, $BG_COLOR+0.02);
		cairo_fill(cr);
EOF
		if($ALIGNMENT_CROSS eq true){
			print TESTC <<EOF;
			cairo_set_source_rgb(cr, .7, 0, 0);
			cairo_set_line_width(cr, 1);
			cairo_move_to(cr, textwidth+2*$BORDER+$column*($ICONWIDTH+$BORDER)+0.5*$ICONWIDTH, $BORDER+$row*($ICONHEIGHT+$BORDER));
			cairo_line_to(cr, textwidth+2*$BORDER+$column*($ICONWIDTH+$BORDER)+0.5*$ICONWIDTH, $BORDER+$row*($ICONHEIGHT+$BORDER)+$ICONHEIGHT);
			cairo_stroke(cr);
EOF
		}
		print TESTC <<EOF;
		cairo_move_to(cr, textwidth+2*$BORDER+$column*($ICONWIDTH+$BORDER), $BORDER+$row*($ICONHEIGHT+$BORDER)+0.5*$ICONHEIGHT);
		cairo_line_to(cr, textwidth+2*$BORDER+$column*($ICONWIDTH+$BORDER)+$ICONWIDTH, $BORDER+$row*($ICONHEIGHT+$BORDER)+0.5*$ICONHEIGHT);
		cairo_stroke(cr);
		cairo_set_source_rgb(cr, $FG_COLOR, $FG_COLOR, $FG_COLOR);
		$code(cr, textwidth+3*$BORDER+$column*($ICONWIDTH+$BORDER), 2*$BORDER+$row*($ICONHEIGHT+$BORDER), $ICONWIDTH-2*$BORDER, $ICONHEIGHT-2*$BORDER, $direction, $data);
		cairo_restore(cr);
EOF
		$column++;
	}

	$row++;
}

# rest of the .c file
print TESTC <<EOF;
	cairo_destroy(cr);
	cairo_surface_write_to_png(surface, "show_icons_in_paint_c.png");
	cairo_surface_destroy(surface);

	return 0;
}

EOF

# compile & run the .c file
system("gcc `pkg-config --cflags --libs gtk+-3.0 json-glib-1.0 librsvg-2.0` -lm -std=c99 -I$srcdir -o $tempdir/test $tempdir/test.c $srcdir/dtgtk/paint.c");
system("$tempdir/test");

print "show_icons_in_paint_c.png created in the current directory\n";
