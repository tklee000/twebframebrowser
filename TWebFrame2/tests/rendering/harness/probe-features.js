(() => {
  const probes = [
    ['cascade-layers','color','revert-layer'],
    ['custom-properties','--rendering-probe','1'],
    ['calc','width','calc(100% - 1px)'],
    ['minmax-functions','width','clamp(10px, 50vw, 600px)'],
    ['flex','display','flex'],['grid','display','grid'],['subgrid','grid-template-columns','subgrid'],
    ['logical-sizing','inline-size','10px'],['vertical-writing','writing-mode','vertical-rl'],
    ['sticky','position','sticky'],['overflow-clip','overflow','clip'],
    ['individual-transform','translate','1px 2px'],['perspective','perspective','500px'],
    ['clip-path','clip-path','polygon(0 0,100% 0,50% 100%)'],
    ['mask','mask-image','linear-gradient(black,transparent)'],['filter','filter','blur(1px)'],
    ['backdrop-filter','backdrop-filter','blur(1px)'],['container-queries','container-type','inline-size'],
    ['color-mix','color','color-mix(in srgb, red, blue)'],['oklch','color','oklch(50% 0.1 120)'],
    ['text-wrap-balance','text-wrap','balance'],['anchor-positioning','anchor-name','--probe'],
    ['content-visibility','content-visibility','auto'],['scrollbar-gutter','scrollbar-gutter','stable'],
    ['aspect-ratio','aspect-ratio','16 / 9']
  ];
  const features=probes.map(([id,property,value])=>({id,property,value,accepted:CSS.supports(property,value),scope:'syntax acceptance only'}));
  for(const [id,selector] of [['selector-has',':has(div)'],['selector-is',':is(div,span)'],['selector-where',':where(div)'],['selector-nth-of',':nth-child(2n of div)']])
    features.push({id,selector,accepted:CSS.supports('selector('+selector+')'),scope:'syntax acceptance only'});
  return {schemaVersion:1,pageScriptsEnabled:false,readOnly:true,userAgent:navigator.userAgent,features};
})();
