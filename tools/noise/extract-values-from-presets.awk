/"generic poissonian"/ {
	next;
}
/^ *{"/ {
	tail = $0;
	sub(/ *{"/, "", tail);
	label = tail;
	sub(/".*$/, "", label);

	sub(/^[^"]+"[^"]+"[^"]+"[^"]+"[^"]+"[^0-9]+/, "", tail);
	iso = tail;
	sub(/,.*/, "", iso);

	sub(/^[0-9]+ *, *\{/, "", tail);
	a = tail;
	sub(/\}.*/, "", a);
	gsub(/, */, " ", a);

	sub(/.*\{/, "", tail);
	b = tail;
	sub(/\}.*/, "", b);
	gsub(/, */, " ", b);

	print iso " " a " " b " " label;
}
