readarray arr < $1
newfile=${1}new
oldiso=0
idx=0
echo -n $(echo ${arr[0]} | awk -F "0," '{print $1;}') > $newfile
for i in "${arr[@]}"; do
curiso=$(echo $i | cut -d"," -f 4 | uniq | sed 's/^ *//')
if [[ $curiso -gt $oldiso ]]
then
if [[ $idx -gt 0 ]]
then
beg=$(echo $i | awk -F "0," '{print $1;}')
ar=$(echo $ar | awk -v n=$nr '{printf "%.14g",$0/n;}')
ag=$(echo $ag | awk -v n=$nr '{printf "%.14g",$0/n;}')
ab=$(echo $ab | awk -v n=$nr '{printf "%.14g",$0/n;}')
br=$(echo $br | awk -v n=$nr '{printf "%.14g",$0/n;}')
bg=$(echo $bg | awk -v n=$nr '{printf "%.14g",$0/n;}')
bb=$(echo $bb | awk -v n=$nr '{printf "%.14g",$0/n;}')
echo "0, {$ar,$ag,$ab}, {$br,$bg,$bb}}," >> $newfile
echo -n $beg >> $newfile
fi
oldiso=$curiso
ar=0
ag=0
ab=0
br=0
bg=0
bb=0
nr=0
else
#sum them
res=$(echo $i | awk -F "0," '{print $2;}')
ar=$(echo ${res:2} | cut -d"," -f 1 | awk -v a=$ar '{printf "%.14g",a+$0;}')
ag=$(echo $res | cut -d"," -f 2 | awk -v a=$ag '{printf "%.14g",a+$0;}')
x=$(echo $res | cut -d"," -f 3)
ab=$(echo ${x:1:${#x}-2} | awk -v a=$ab '{printf "%.14g",a+$0;}')
res=$(echo $i | awk -F "}, {" '{print $2;}')
br=$(echo $res | cut -d"," -f 1 | awk -v a=$br '{printf "%.14g",a+$0;}')
bg=$(echo $res | cut -d"," -f 2 | awk -v a=$bg '{printf "%.14g",a+$0;}')
x=$(echo $res | cut -d"," -f 3)
bb=$(echo ${x:1:${#x}-3} | awk -v a=$bb '{printf "%.14g",a+$0;}')
nr=$(($nr+1))
fi
idx=$(($idx+1))
done;
ar=$(echo $ar | awk -v n=$nr '{printf "%.14g",$0/n;}')
ag=$(echo $ag | awk -v n=$nr '{printf "%.14g",$0/n;}')
ab=$(echo $ab | awk -v n=$nr '{printf "%.14g",$0/n;}')
br=$(echo $br | awk -v n=$nr '{printf "%.14g",$0/n;}')
bg=$(echo $bg | awk -v n=$nr '{printf "%.14g",$0/n;}')
bb=$(echo $bb | awk -v n=$nr '{printf "%.14g",$0/n;}')
echo "0, {$ar,$ag,$ab}, {$br,$bg,$bb}}," >> $newfile
echo "" >> $newfile