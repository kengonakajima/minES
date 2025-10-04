// AMDF demo with two short arrays (y is x delayed by ~1 sample)
const x = [0,1,2,3,2,1,0,8,1,9];
//         ^^^^^^^^^^^  
const y = [2,4,8,0,0,1,2,3,2,1];
//                 ^^^^^^^^^^^
function amdf(x,y,l) { // l = lag
  let s=0,c=0;
  for (let n=0; n<x.length; n++) {
    const m=n+l;
    if (m>=0 && m<y.length) {
      s += Math.abs(x[n]-y[m]);
      c++;
    }
  }
  if(c==0) c=1;
  return s / c;
};

let bestLag=0, best=Infinity;
for (let l=-3; l<=3; l++) { const v=amdf(x,y,l);
  console.log(`lag=${l}  AMDF=${v.toFixed(3)}`);
  if (v<best) best=v, bestLag=l;
}
console.log('best lag =', bestLag);

