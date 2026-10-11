using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Security.Cryptography;

public sealed class PeAudit
{
    public sealed class Section { public string Name; public uint Rva, VirtualSize, Raw, RawSize, Flags; }
    public byte[] Data;
    public ulong ImageBase;
    public string Hash;
    public uint ImageSize;
    public sealed class FunctionRange { public uint Start, End, Parent; }
    public List<FunctionRange> Functions = new List<FunctionRange>();
    private uint exportRva;
    public List<Section> Sections = new List<Section>();
    public PeAudit(string path) {
        Data=File.ReadAllBytes(path);
        using(var sha=SHA256.Create()) Hash=BitConverter.ToString(sha.ComputeHash(Data)).Replace("-", "").ToLowerInvariant();
        int pe=BitConverter.ToInt32(Data,0x3c), count=BitConverter.ToUInt16(Data,pe+6), opt=BitConverter.ToUInt16(Data,pe+20);
        if (BitConverter.ToUInt16(Data,0)!=0x5a4d || BitConverter.ToUInt32(Data,pe)!=0x4550 || BitConverter.ToUInt16(Data,pe+24)!=0x20b)
            throw new InvalidDataException("Expected a PE64 module");
        ImageBase=BitConverter.ToUInt64(Data,pe+24+24);
        ImageSize=BitConverter.ToUInt32(Data,pe+24+56);
        exportRva=BitConverter.ToUInt32(Data,pe+24+112);
        int table=pe+24+opt;
        for(int i=0;i<count;i++) { int p=table+i*40; Sections.Add(new Section {
            Name=Encoding.ASCII.GetString(Data,p,8).TrimEnd('\0'), VirtualSize=BitConverter.ToUInt32(Data,p+8),
            Rva=BitConverter.ToUInt32(Data,p+12), RawSize=BitConverter.ToUInt32(Data,p+16), Raw=BitConverter.ToUInt32(Data,p+20), Flags=BitConverter.ToUInt32(Data,p+36)}); }
        uint pdata=BitConverter.ToUInt32(Data,pe+24+112+3*8), size=BitConverter.ToUInt32(Data,pe+24+116+3*8);
        if(pdata!=0) for(uint i=0;i+12<=size;i+=12) {
            uint start=U32(pdata+i),end=U32(pdata+i+4),parent=0,unwind=U32(pdata+i+8);
            if(start<end && end<=ImageSize) {
                try {int p=Raw(unwind);if(((Data[p]>>3)&4)!=0)parent=U32(unwind+4+(uint)(((Data[p+2]+1)&~1)*2));}catch{}
                Functions.Add(new FunctionRange{Start=start,End=end,Parent=parent});
            }
        }
    }
    public int Raw(uint rva) { foreach(var s in Sections) if(rva>=s.Rva && rva-s.Rva<s.RawSize) return checked((int)(s.Raw+rva-s.Rva)); throw new ArgumentOutOfRangeException("rva"); }
    public uint Rva(int raw) { foreach(var s in Sections) if(raw>=s.Raw && raw-s.Raw<s.RawSize) return s.Rva+(uint)raw-s.Raw; throw new ArgumentOutOfRangeException("raw"); }
    public byte[] Bytes(uint rva,int count) { int p=Raw(rva); var b=new byte[count]; Array.Copy(Data,p,b,0,count);return b; }
    public ulong Pointer(uint rva) {return BitConverter.ToUInt64(Data,Raw(rva));}
    public uint U32(uint rva) {return BitConverter.ToUInt32(Data,Raw(rva));}
    public string Ascii(uint rva) {int p=Raw(rva),e=p;while(e<Data.Length && Data[e]!=0 && e-p<512)e++;return Encoding.ASCII.GetString(Data,p,e-p);}
    public uint Relative(uint instruction,int operand,int size) {return checked((uint)((long)instruction+size+BitConverter.ToInt32(Data,Raw(instruction)+(int)operand)));}
    public FunctionRange FunctionAt(uint rva) {
        int lo=0,hi=Functions.Count-1;
        while(lo<=hi) {int m=(lo+hi)/2;var f=Functions[m];if(rva<f.Start)hi=m-1;else if(rva>=f.End)lo=m+1;else return f;}
        return null;
    }
    public List<FunctionRange> FunctionPieces(uint rva) {
        var original=FunctionAt(rva);if(original==null)return new List<FunctionRange>();
        uint root=FunctionRoot(original);
        var pieces=new List<FunctionRange>();foreach(var f in Functions)if(FunctionRoot(f)==root)pieces.Add(f);
        pieces.Sort((a,b)=>a.Start.CompareTo(b.Start));return pieces;
    }
    private uint FunctionRoot(FunctionRange range) {
        for(int depth=0;range.Parent!=0 && depth<32;depth++) {var parent=FunctionAt(range.Parent);if(parent==null || parent.Start==range.Start)break;range=parent;}
        return range.Start;
    }
    public bool Contains(uint rva,bool executable) {foreach(var s in Sections)if(rva>=s.Rva && rva-s.Rva<s.VirtualSize)return !executable || (s.Flags&0x20000000)!=0;return false;}
    public bool Matches(uint rva,int[] pattern) {try{var b=Bytes(rva,pattern.Length);for(int i=0;i<b.Length;i++)if(pattern[i]>=0 && pattern[i]!=b[i])return false;return true;}catch{return false;}}
    public uint Export(string name) {
        if(exportRva==0)return 0;
        uint count=U32(exportRva+24),names=U32(exportRva+32),ordinals=U32(exportRva+36),addresses=U32(exportRva+28);
        for(uint i=0;i<count;i++)if(Ascii(U32(names+i*4))==name){ushort ordinal=BitConverter.ToUInt16(Data,Raw(ordinals+i*2));return U32(addresses+ordinal*4u);}
        return 0;
    }
    public uint RttiOffset(uint table) {try{return U32((uint)(Pointer(table-8)-ImageBase)+4);}catch{return uint.MaxValue;}}
    public List<uint> Find(int[] pattern,bool executableOnly) {
        var results=new List<uint>();
        foreach(var s in Sections) {
            if((s.Flags&0x40000000)==0 || (s.Flags&0x02000000)!=0 || (executableOnly && (s.Flags&0x20000000)==0))continue;
            int length=(int)Math.Min(s.RawSize,s.VirtualSize),p=(int)s.Raw;
            for(int i=0;i<=length-pattern.Length;i++) {
                int j=0;for(;j<pattern.Length;j++)if(pattern[j]>=0 && Data[p+i+j]!=pattern[j])break;
                if(j==pattern.Length)results.Add(s.Rva+(uint)i);
            }
        }
        return results;
    }
    public string Rtti(uint table) {try {ulong col=Pointer(table-8); if(col<ImageBase || col-ImageBase>0x8000000)return "";uint crva=(uint)(col-ImageBase); if(U32(crva)!=1)return "";uint td=U32(crva+12);return Ascii(td+16);}catch{return "";} }
    public List<uint> Vtables(string typeName) {
        var found=new List<uint>(); foreach(var s in Sections) {if(s.Name!=".rdata")continue; for(uint r=s.Rva+8;r+8<s.Rva+Math.Min(s.RawSize,s.VirtualSize);r+=8) {if(Rtti(r)==typeName)found.Add(r);}} return found;
    }
}
