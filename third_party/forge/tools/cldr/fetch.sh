B=https://raw.githubusercontent.com/unicode-org/cldr-json/46.0.0/cldr-json
L="en en-GB en-AU en-IN en-CA de de-CH fr fr-CA es es-MX it pt pt-PT nl sv da nb fi pl cs ru uk tr el he ar fa hi bn th vi id ja ko zh zh-Hant ro hu sk bg hr ga cy ms sw et lt lv sl sr ca"
for l in $L; do mkdir -p $l
 for p in "cldr-numbers-full/main/$l/numbers.json" "cldr-numbers-full/main/$l/currencies.json" "cldr-dates-full/main/$l/ca-gregorian.json" "cldr-dates-full/main/$l/dateFields.json" "cldr-misc-full/main/$l/listPatterns.json" "cldr-units-full/main/$l/units.json" "cldr-localenames-full/main/$l/languages.json"; do
  curl -sfL "$B/$p" -o "$l/$(basename $p)" || echo "FAIL $l $p"; done; done
mkdir -p supp; for s in plurals ordinals weekData measurementData currencyData likelySubtags; do curl -sfL $B/cldr-core/supplemental/$s.json -o supp/$s.json || echo FAIL $s; done
echo DONE
