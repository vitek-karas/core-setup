// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.IO;

namespace Microsoft.DotNet.CoreSetup.Test.HostActivation
{
    public static class TestOnlyProductBehavior
    {
        public static IDisposable Enable(string productPath)
        {
            string directoryPath;
            if (Directory.Exists(productPath))
            {
                directoryPath = productPath;
            }
            else
            {
                directoryPath = Path.GetDirectoryName(productPath);
            }

            string testOnlyFileMarkPath = Path.Combine(directoryPath, Constants.TestOnlyFile.FileName);
            File.WriteAllText(testOnlyFileMarkPath, "");
            return new TestOnlyEnableFileMark(testOnlyFileMarkPath);
        }

        private class TestOnlyEnableFileMark : IDisposable
        {
            private string _path;

            public TestOnlyEnableFileMark(string path)
            {
                _path = path;
            }

            public void Dispose()
            {
                if (_path != null && File.Exists(_path))
                {
                    File.Delete(_path);
                }
            }
        }
    }
}
