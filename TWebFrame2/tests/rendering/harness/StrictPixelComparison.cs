using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

public sealed class RenderingPixelResult {
    public int Width, Height, DifferentPixels, MaximumChannelDelta;
    public int Left=-1,Top=-1,Right=-1,Bottom=-1;
}
public static class StrictRenderingPixels {
    // Exception signatures identify decoded pixels, independently of PNG encoding.
    public static string PixelSha256(string path) {
        using(var bitmap=new Bitmap(path)) using(var sha=SHA256.Create()) {
            return BitConverter.ToString(sha.ComputeHash(Pixels(bitmap))).Replace("-", "");
        }
    }
    static byte[] Pixels(Bitmap source) {
        using(var copy=new Bitmap(source.Width,source.Height,PixelFormat.Format32bppArgb)) {
            using(var graphics=Graphics.FromImage(copy)) {
                graphics.CompositingMode=System.Drawing.Drawing2D.CompositingMode.SourceCopy;
                graphics.DrawImageUnscaled(source,0,0);
            }
            var data=copy.LockBits(new Rectangle(0,0,copy.Width,copy.Height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
            try {
                var bytes=new byte[copy.Width*copy.Height*4];
                for(int y=0;y<copy.Height;y++) Marshal.Copy(IntPtr.Add(data.Scan0,y*data.Stride),bytes,y*copy.Width*4,copy.Width*4);
                return bytes;
            } finally { copy.UnlockBits(data); }
        }
    }
    public static RenderingPixelResult Compare(string reference,string actual,string diffPath) {
        using(var expected=new Bitmap(reference)) using(var native=new Bitmap(actual)) {
            if(expected.Width!=native.Width||expected.Height!=native.Height) throw new InvalidOperationException("Image dimensions differ");
            var a=Pixels(expected);var b=Pixels(native);var diff=new byte[a.Length];
            var result=new RenderingPixelResult {Width=expected.Width,Height=expected.Height};
            for(int y=0;y<expected.Height;y++) for(int x=0;x<expected.Width;x++) {
                int i=(y*expected.Width+x)*4;bool different=false;int maximum=0;
                for(int c=0;c<4;c++) { int d=Math.Abs(a[i+c]-b[i+c]);if(d>0)different=true;if(d>maximum)maximum=d; }
                diff[i+3]=255;
                if(different) {
                    result.DifferentPixels++;result.MaximumChannelDelta=Math.Max(result.MaximumChannelDelta,maximum);
                    if(result.Left<0){result.Left=result.Right=x;result.Top=result.Bottom=y;}
                    result.Left=Math.Min(result.Left,x);result.Right=Math.Max(result.Right,x);
                    result.Top=Math.Min(result.Top,y);result.Bottom=Math.Max(result.Bottom,y);
                    diff[i]=0;diff[i+1]=0;diff[i+2]=255;
                } else { byte g=(byte)((a[i]+a[i+1]+a[i+2])/6+96);diff[i]=diff[i+1]=diff[i+2]=g; }
            }
            if(!String.IsNullOrEmpty(diffPath)) using(var bitmap=new Bitmap(expected.Width,expected.Height,PixelFormat.Format32bppArgb)) {
                var data=bitmap.LockBits(new Rectangle(0,0,bitmap.Width,bitmap.Height),ImageLockMode.WriteOnly,PixelFormat.Format32bppArgb);
                try{for(int y=0;y<bitmap.Height;y++)Marshal.Copy(diff,y*bitmap.Width*4,IntPtr.Add(data.Scan0,y*data.Stride),bitmap.Width*4);}
                finally{bitmap.UnlockBits(data);}bitmap.Save(diffPath,ImageFormat.Png);
            }
            return result;
        }
    }
}
