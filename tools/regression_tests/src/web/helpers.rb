TYPES = [
  [:new_pass, {:name => "New Pass", :longname => "File now works", :class => "success"}],
  [:still_pass, {:name => "Still Pass", :longname => "File still works"}],
  [:still_fail, {:name => "Still Fail", :longname => "File now fails", :class => "warning"}],
  [:warn, {:name => "Warning", :longname => "File has some small differences", :class => "warning", :diffed => true}],
  [:diff, {:name => "Difference", :longname => "File has substantial differences", :class => "danger", :diffed => true}],
  [:new_fail, {:name => "New Fail", :longname => "File now fails", :class => "danger"}],
]

TYPEHASH = {}
TYPES.each {|type, opts| TYPEHASH[type] = opts}
