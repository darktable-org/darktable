%\nonstopmode
\ProvidesClass{photobook}[2012/05/10 darktable book print]
\NeedsTeXFormat{LaTeX2e}
\LoadClass[10pt,final,openany]{book}

\DeclareOption{draftmode}{\newcommand{\draftmode}{}}

\ProcessOptions\relax


\RequirePackage{ifthen}
\RequirePackage{calc}
\RequirePackage{graphicx}
\ifthenelse{\isundefined{\draftmode}}{
\graphicspath{{./images/}}
}{
\graphicspath{{./thumbnails/}}
}

\frenchspacing
\RequirePackage{microtype}

% bera comes with texlive-fonts-extra
\RequirePackage[T1]{fontenc}
\RequirePackage{ae,aecompl}
\RequirePackage{textcomp}
\RequirePackage[scaled=0.95]{berasans}
\RequirePackage[scaled=0.95]{beraserif}
\RequirePackage[scaled=0.95]{beramono}
\renewcommand{\familydefault}{\sfdefault}

\RequirePackage{pict2e}
\RequirePackage{color}

\setlength{\unitlength}{1mm}

% have a nice framebox with border directly around the image:
\fboxsep 0pt
\newcommand{\fimg}[2]{\fbox{\includegraphics[width=#1]{#2}}}

\definecolor{darkblue}{rgb}{0.,0.,0.25}
\RequirePackage{hyperref}
\hypersetup{pdftitle={\dttitle},
  pdfsubject={\dtsubject},
  pdfauthor={\dtauthor},
  pdfkeywords={\dtkeywords},
  pdfpagemode={},
  pdfborder={0 0 0 0 0 0},
  colorlinks={true},
  linkcolor={darkblue},
  citecolor={darkblue},
  filecolor={black},
  urlcolor={black}
}


% standard landscape

% front cover
% 9.76 x 8.25 in
% (24.80 x 20.96 cm)
  
% trimmed pages
% 9.63 x 8.00 in
% (24.45 x 20.32 cm)

% print that on letter format:
% 279.4 x 215.9 mm

% trim (and that again for secure border to important content)
% 3.175mm

\RequirePackage[paperwidth=279.4mm, paperheight=215.9mm,   % letter
            % bindingoffset=31.725mm,  % would be all the rest,
            % but we want it to be centered, so we can use a global trim size
            % set total to the dimensions of the trimmed part (as required above)
            total={244.5mm,203.2mm},
            % include header/footer/margin notes in printed area
            twoside, includeall, nomarginpar,
            ignorehead=false, ignorefoot=false, ignoremp=false,
            % center printed area on page
            vcentering, hcentering]{geometry}

\setlength{\footnotesep}{0pt}
\setlength{\headheight}{0pt}
\setlength{\headsep}{0pt}

\newcommand{\paperwidthnum}{244.5} % in mm, does not include binding offset
\newcommand{\paperheightnum}{203.2} % in mm
\newcommand{\trimnum}{3.175}
\newcommand{\paperwidthplustrimnum}{247.675} % in mm, does not include binding offset
\newcommand{\paperheightplustrimnum}{206.375} % in mm
\newcommand{\imgwidth}{244.5mm}
\newcommand{\imgheight}{203.2mm}

\newcommand{\drawtrimcorners}{%
  \moveto(-\trimnum, 0)
  \lineto(0, 0)
  \strokepath
  \moveto(0, \trimnum)
  \lineto(0, 0)
  \strokepath

  \moveto(\paperwidthplustrimnum, -\paperheightnum)
  \lineto(\paperwidthnum, -\paperheightnum)
  \strokepath
  \moveto(\paperwidthnum, -\paperheightplustrimnum)
  \lineto(\paperwidthnum, -\paperheightnum)
  \strokepath

  \moveto(-\trimnum, -\paperheightnum)
  \lineto(0, -\paperheightnum)
  \strokepath
  \moveto(0, -\paperheightplustrimnum)
  \lineto(0, -\paperheightnum)
  \strokepath

  \moveto(\paperwidthplustrimnum, 0)
  \lineto(\paperwidthnum, 0)
  \strokepath
  \moveto(\paperwidthnum, \trimnum)
  \lineto(\paperwidthnum, 0)
  \strokepath
}

% avoid silly spaces before our picture environments.
\parindent0pt
\parskip0pt

\title{\dttitle}
\author{\dtauthor}

%%%%%%%%% PDF/X-3 stuff, necessary for Blurb IF USING pdflatex %%%%%%%%%
% ICC color profiles are embedded in the images
\pdfinfo{
/Title (\dttitle)   % set your title here
/Author (\dtauthor)       % set author name
/Subject (\dtsubject)          % set subject
/Keywords (\dtkeywords) % set keywords
/Trapped (False)
/GTS_PDFXVersion (PDF/X-3:2002)
}
% pdf unit is 1/72 inch or 25.4/72 mm
% PDF/X-3 files contain extra operators that define the bleed and trim area.
% - The MediaBox defines the size of the entire document
% - The ArtBox or TrimBox defines the extent of the printable area.
% - If the file is to be printed with bleed, a BleedBox must be defined. It must be larger than the TrimBox/ArtBox, but smaller than the MediaBox.
\ifthenelse{\isundefined{\draftmode}}{
\pdfpageattr{/MediaBox [0 0 792.0 612.0]
/TrimBox [49.4645668785 18.0 742.535433 594.0]}
}{
\pdfpageattr{/MediaBox [0 0 792.0 612.0]
/CropBox [49.4645668785 18.0 742.535433 594.0]}
}
% test with:
% convert -define pdf:use-trimbox=true header.pdf cropped.png
% or by passing [draftmode] to this class.
\pdfminorversion=3
\pdfcatalog{
/OutputIntents [ <<
/Info (none)
/Type /OutputIntent
/S /GTS_PDFX
/OutputConditionIdentifier (Blurb.com)
/RegistryName (http://www.color.org/)
>> ]
}

% http://blurb.custhelp.com/app/answers/detail/a_id/474/related/1/session/L2F2LzEvdGltZS8xMzM0NDQ5ODMyL3NpZC9nQjFIbkZWaw%3D%3D

% http://www.blurb.com/create/book/dimensions


% first page is left page, cover page is extra.
