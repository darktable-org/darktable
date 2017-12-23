#!/usr/bin/env ruby

# MIT License
#
# Copyright (c) 2016 Roman Lebedev
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# minimal number of commits an author should have, to be listed at all
SHORTLOG_THRESHOLD = 1

# minimal number of commits translator should have, to be listed
TRANSLATOR_THRESHOLD = 1

# minimal number of commits regular contributor should have, to be listed
CONTRIBUTOR_THRESHOLD = 4

# these will not be shown in contributors section.
ALL_DEVELOPERS = [
    "Aldric Renaudin",
    "Alexandre Prokoudine",
    "Christian Tellefsen",
    "Edouard Gomez",
    "Henrik Andersson",
    "James C. McPherson",
    "José Carlos García Sogo",
    "Jérémy Rosen",
    "Pascal Obry",
    "Pascal de Bruijn",
    "Pedro Côrte-Real",
    "Peter Budai",
    "Roman Lebedev",
    "Simon Spannagel",
    "Stefan Schöfegger",
    "Tobias Ellinghaus",
    "Ulrich Pegelow",
    "johannes hanika",
    "parafin"
  ]

VERSIONS = ARGV[0]

def get_shortlog(path="")
  hash = {}

  IO.popen("git shortlog -sn #{VERSIONS} -- #{path}").each do |line|
    parts = line.strip().split("\t")
    hash[parts[1].to_s] = parts[0].to_i
  end

  return hash
end

SHORTLOG = get_shortlog().keep_if{ |authorname, count| count >= SHORTLOG_THRESHOLD }

# all developers, that made any changes in selected timeframe
DEVELOPERS = SHORTLOG.select{ |authorname, count| ALL_DEVELOPERS.include?(authorname) }

# all the people that changed PO files
TRANSLATORS = get_shortlog("./po/*.po ./doc/man/po/*.po ./doc/usermanual/po/*.po") \
# and keep only the ones with at least this much commits
  .keep_if { |authorname, count| count >= TRANSLATOR_THRESHOLD }

# all contributors (except those in dev list, but including translators)
CONTRIBUTORS = SHORTLOG.reject{ |authorname, count| ALL_DEVELOPERS.include?(authorname) } \
# and keep only the ones with at least this much commits
  .keep_if{ |authorname, count| count >= CONTRIBUTOR_THRESHOLD }

puts "* developers:"
puts DEVELOPERS.keys.join(",\n") + "."
puts

puts "* translators:"
puts TRANSLATORS.keys.join(",\n") + "."
puts

puts "* contributors (at least #{CONTRIBUTORS.values.min} commits):"
puts CONTRIBUTORS.keys.join(",\n") + "."
puts

puts "FIXME: account for rawspeed.\n\n"

puts "And all those of you that made previous releases possible"

# vim: tabstop=2 expandtab shiftwidth=2 softtabstop=2
# kate: tab-width: 2; replace-tabs on; indent-width 2; tab-indents: off;
# kate: indent-mode ruby; remove-trailing-spaces modified;
