export function getPolyline(data, height, minVal, maxVal) {
    if (!data || data.length < 2) return ""; 

    const width = 100;
    const step = width / (data.length - 1);

    let min = minVal;
    let max = maxVal;
    
    // Auto-scale if min/max not provided
    if (min === undefined || max === undefined) {
        const validData = data.filter(n => Number.isFinite(n));
        
        if (validData.length === 0) {
            min = 0; max = 100; 
        } else {
            min = Math.min(...validData);
            max = Math.max(...validData);
        }
        
        if (min === max) { max += 1; min -= 1; }
    }
    
    const range = max - min;
    
    return data.map((val, i) => {
      if (!Number.isFinite(val)) return `${i * step},${height}`; 
      
      const x = i * step;
      const clampedVal = Math.max(min, Math.min(max, val));
      const y = height - ((clampedVal - min) / range) * height; 
      return `${x},${y}`;
    }).join(" ");
}
