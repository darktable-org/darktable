plot './iso100.dat' u 1:2 w lp, '' u 1:3 w lp, '' u 1:4 w lp

f(x) = a1*x + b1
a1=1;b1=0;
# TODO: only plot right values!
set xrange [0, 220]
fit f(x) './iso100.dat' u 1:($2*$2) via a1, b1
set xrange [0, 300]
plot './iso100.dat' u 1:2 w lp, '' u 1:3 w lp, '' u 1:4 w lp, sqrt(f(x))
